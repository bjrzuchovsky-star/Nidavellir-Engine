// Engine side of C# scripting: entry points the native host gets through
// hostfxr (load_assembly_and_get_function_pointer, UnmanagedCallersOnly).
// Each build of the project's scripts is one generation in its own
// collectible AssemblyLoadContext; hot reload loads a new generation and
// retires the old one, which unloads once its last script is destroyed.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using TombStone.Native;

namespace TombStone
{
    public static unsafe class Host
    {
        sealed class ScriptsContext : AssemblyLoadContext
        {
            public ScriptsContext(int id) : base("TombStoneScripts-" + id, isCollectible: true) { }
            protected override Assembly Load(AssemblyName name) =>
                name.Name == typeof(Script).Assembly.GetName().Name ? typeof(Script).Assembly : null;
        }

        sealed class Generation
        {
            public ScriptsContext Context;
            public WeakReference Alive;
            public Dictionary<string, Type> Classes = new Dictionary<string, Type>(StringComparer.OrdinalIgnoreCase);
            public int Live;
            public bool Retired;
        }

        sealed class Instance
        {
            public Script Script;
            public Generation Gen;
        }

        static readonly Dictionary<int, Generation> gens = new Dictionary<int, Generation>();
        static readonly Dictionary<ulong, List<Script>> byEntity = new Dictionary<ulong, List<Script>>();
        static readonly List<Script> runtimeAdded = new List<Script>();
        static readonly HashSet<Script> runtimeStarted = new HashSet<Script>();
        static int pumpedFrame = -1;
        static int pumpedMask;

        internal static IEnumerable<Script> ScriptsOn(ulong id)
        {
            List<Script> list;
            if (byEntity.TryGetValue(id, out list))
                foreach (Script s in list) yield return s;
            foreach (Script s in runtimeAdded)
                if (s.Self == id) yield return s;
        }
        internal static void AddRuntime(Script s)
        {
            runtimeAdded.Add(s);
            Script prev = Script.Active;
            Script.Active = s;
            try { s.Awake(); s.OnEnable(); }
            catch (Exception e) { s.Error(s.GetType().Name + ": " + e.Message); }
            finally { Script.Active = prev; }
        }
        internal static void RemoveRuntime(Script s)
        {
            if (!runtimeAdded.Remove(s)) return;
            try { s.OnDisable(); s.OnDestroy(); }
            catch (Exception e) { s.Error("remove: " + e.Message); }
        }
        internal static void PumpRuntime(int phase, float dt)
        {
            int bit = 1 << phase;
            if (pumpedFrame != Time.Frame) { pumpedFrame = Time.Frame; pumpedMask = 0; }
            if ((pumpedMask & bit) != 0) return;
            pumpedMask |= bit;
            Script[] copy = runtimeAdded.ToArray();
            foreach (Script s in copy)
            {
                Script prev = Script.Active;
                Script.Active = s;
                try
                {
                    if (phase == 0) s.OnFixedUpdate(dt);
                    else if (phase == 1)
                    {
                        if (runtimeStarted.Add(s)) s.OnStart();
                        s.OnTick(dt);
                    }
                    else s.OnLateUpdate(dt);
                }
                catch (Exception e) { s.Error(s.GetType().Name + ": " + e.Message); }
                finally { Script.Active = prev; }
            }
        }
        static readonly List<WeakReference> unloading = new List<WeakReference>();
        static int nextGen = 1;

        static void Write(string s, byte* buf, int cap)
        {
            if (buf == null || cap <= 0) return;
            byte[] b = System.Text.Encoding.UTF8.GetBytes(s);
            int n = Math.Min(b.Length, cap - 1);
            Marshal.Copy(b, 0, (IntPtr)buf, n);
            buf[n] = 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Abi() => 4;

        // Loads the project's script assembly from bytes (the file stays
        // free for the next build). Generation id, 0 on error (text in err).
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int LoadScripts(byte* path, byte* err, int cap) => LoadImpl(Utf8.Read(path), err, cap);

        [MethodImpl(MethodImplOptions.NoInlining)]
        static int LoadImpl(string p, byte* err, int cap)
        {
            try
            {
                int id = nextGen++;
                var g = new Generation { Context = new ScriptsContext(id) };
                g.Alive = new WeakReference(g.Context);
                Assembly asm;
                using (var dll = new MemoryStream(File.ReadAllBytes(p)))
                {
                    string pdb = Path.ChangeExtension(p, ".pdb");
                    if (File.Exists(pdb))
                        using (var sym = new MemoryStream(File.ReadAllBytes(pdb))) asm = g.Context.LoadFromStream(dll, sym);
                    else asm = g.Context.LoadFromStream(dll);
                }
                foreach (Type t in asm.GetTypes())
                {
                    if (t.IsAbstract || !typeof(Script).IsAssignableFrom(t)) continue;
                    var a = t.GetCustomAttribute<ScriptAttribute>();
                    if (a != null) g.Classes[a.Stem] = t;
                    g.Classes[t.Name] = t;
                }
                gens[id] = g;
                return id;
            }
            catch (Exception e)
            {
                Write(e.GetType().Name + ": " + e.Message, err, cap);
                return 0;
            }
        }

        static void MaybeUnload(Generation g)
        {
            if (!g.Retired || g.Live != 0 || g.Context == null) return;
            g.Classes.Clear();
            g.Context.Unload();
            g.Context = null;
            unloading.Add(g.Alive);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void UnloadScripts(int id)
        {
            if (!gens.TryGetValue(id, out Generation g)) return;
            g.Retired = true;
            gens.Remove(id);
            MaybeUnload(g);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static IntPtr Create(int gen, byte* cls, TsScriptApi* api, ulong self, byte* props)
        {
            Script s = null;
            try
            {
                if (!gens.TryGetValue(gen, out Generation g)) return IntPtr.Zero;
                string stem = Utf8.Read(cls);
                if (!g.Classes.TryGetValue(stem, out Type t) && !g.Classes.TryGetValue(stem.Replace("_", ""), out t))
                    return IntPtr.Zero;
                s = (Script)Activator.CreateInstance(t);
                s.api = api;
                s.Self = self;
                foreach (string line in Utf8.Read(props).Split('\n'))
                {
                    string[] f = line.Split('\t');
                    if (f.Length < 3) continue;
                    var v = new Value { Type = f[1].Length > 0 ? f[1][0] : 'n', Text = f[2] };
                    if (v.Type == 'n') double.TryParse(f[2], NumberStyles.Float, CultureInfo.InvariantCulture, out v.Number);
                    if (v.Type == 'b') v.Number = f[2] == "1" || f[2] == "true" ? 1 : 0;
                    s.props[f[0]] = v;
                }
                g.Live++;
                List<Script> list;
                if (!byEntity.TryGetValue(self, out list)) byEntity[self] = list = new List<Script>();
                list.Add(s);
                Script prev = Script.Active;
                Script.Active = s;
                try { s.Awake(); s.OnEnable(); }
                catch (Exception e) { s.Error(s.GetType().Name + " awake: " + e.Message); }
                finally { Script.Active = prev; }
                return GCHandle.ToIntPtr(GCHandle.Alloc(new Instance { Script = s, Gen = g }));
            }
            catch (Exception e)
            {
                if (s != null) s.Error("C# create: " + e.Message);
                return IntPtr.Zero;
            }
        }

        static Instance Get(IntPtr h) => h == IntPtr.Zero ? null : GCHandle.FromIntPtr(h).Target as Instance;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void Destroy(IntPtr h)
        {
            Instance i = Get(h);
            if (i == null) return;
            if (byEntity.TryGetValue(i.Script.Self, out List<Script> list)) list.Remove(i.Script);
            GCHandle.FromIntPtr(h).Free();
            i.Gen.Live--;
            MaybeUnload(i.Gen);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void Call(IntPtr h, int hook, ulong entity, double number, byte* text)
        {
            Instance i = Get(h);
            if (i == null) return;
            Script s = i.Script;
            Script prev = Script.Active;
            Script.Active = s;
            try
            {
                switch (hook)
                {
                    case 0: s.OnStart(); break;
                    case 1:
                        Time.DeltaTime = (float)number;
                        Time.Elapsed += (float)number;
                        PumpRuntime(1, (float)number);
                        s.OnTick(number);
                        break;
                    case 10:
                        Time.DeltaTime = (float)number;
                        Time.Frame++;
                        PumpRuntime(0, (float)number);
                        s.OnFixedUpdate((float)number);
                        break;
                    case 11:
                        Time.DeltaTime = (float)number;
                        PumpRuntime(2, (float)number);
                        s.OnLateUpdate((float)number);
                        break;
                    case 12:
                    {
                        string phase = Utf8.Read(text);
                        if (phase == "enter") s.OnCollisionEnter(entity);
                        else if (phase == "stay") s.OnCollisionStay(entity);
                        else s.OnCollisionExit(entity);
                        break;
                    }
                    case 2: s.OnTriggerEnter(entity); break;
                    case 3: s.OnTriggerExit(entity); break;
                    case 4: s.OnInteract(entity); break;
                    case 5: s.OnReload(); break;
                    case 6: s.OnDisable(); s.OnDestroy(); break;
                    case 7:
                    {
                        string[] f = Utf8.Read(text).Split('\t');
                        s.OnHand(f[0], f.Length > 1 ? f[1] : "", number);
                        break;
                    }
                    case 8:
                    {
                        string t = Utf8.Read(text);
                        int tab = t.IndexOf('\t');
                        s.OnUi(tab < 0 ? t : t.Substring(0, tab), entity != 0, number, tab < 0 ? "" : t.Substring(tab + 1));
                        break;
                    }
                    case 9:
                    {
                        string t = Utf8.Read(text);
                        int tab = t.IndexOf('\t');
                        s.OnEvent(tab < 0 ? t : t.Substring(0, tab), tab < 0 ? "" : t.Substring(tab + 1), number);
                        break;
                    }
                }
            }
            catch (Exception e)
            {
                s.Error(s.GetType().Name + ": " + e.GetType().Name + ": " + e.Message + Where(e));
            }
            finally
            {
                Script.Active = prev;
            }
        }

        static string Where(Exception e)
        {
            var st = new System.Diagnostics.StackTrace(e, true);
            foreach (var f in st.GetFrames() ?? Array.Empty<System.Diagnostics.StackFrame>())
            {
                string file = f.GetFileName();
                if (!string.IsNullOrEmpty(file)) return " (at " + file + ":" + f.GetFileLineNumber() + ")";
            }
            return "";
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Save(IntPtr h, byte* buf, int cap)
        {
            Instance i = Get(h);
            if (i == null) return 0;
            try
            {
                byte[] b = System.Text.Encoding.UTF8.GetBytes(i.Script.Save() ?? "");
                if (buf != null && cap >= b.Length) Marshal.Copy(b, 0, (IntPtr)buf, b.Length);
                return b.Length;
            }
            catch (Exception e)
            {
                i.Script.Error("C# save: " + e.Message);
                return 0;
            }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void Load(IntPtr h, byte* data, int len)
        {
            Instance i = Get(h);
            if (i == null) return;
            try { i.Script.Load(Utf8.Read(data, len)); }
            catch (Exception e) { i.Script.Error("C# load: " + e.Message); }
        }

        // Load contexts not yet collected (loaded + retired ones still
        // unloading), after a full GC. The smoke checks a retired one goes.
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int LiveContexts()
        {
            for (int k = 0; k < 8 && unloading.Exists(w => w.IsAlive); ++k)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            unloading.RemoveAll(w => !w.IsAlive);
            return gens.Count + unloading.Count;
        }
    }
}
