// TombStone C# scripts. Write gameplay scripts in <project>/scripts/*.cs:
//
//   using TombStone;
//   [Script("door")]                       // file stem; or name the class Door
//   public class Door : Script {
//       int opened;
//       public override void OnStart() => Log("door ready, reach " + Prop("reach", 24.0));
//       public override void OnInteract(ulong rider) { opened++; Toast("The door creaks"); Set("open", true); }
//       public override string Save() => opened.ToString();      // kept across hot reload
//       public override void Load(string s) => int.TryParse(s, out opened);
//   }
//
// The engine builds every scripts/*.cs into one assembly with the .NET SDK
// (dotnet build) and hot-reloads it by unloading the old collectible
// AssemblyLoadContext. Same hooks and world API as Lua, graphs and C++.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using TombStone.Native;

namespace TombStone
{
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class ScriptAttribute : Attribute
    {
        public string Stem { get; }
        public ScriptAttribute(string stem) { Stem = stem; }
    }

    [Flags]
    public enum Button : uint
    {
        Action = 1u << 0, Alt = 1u << 1, Start = 1u << 2, Use = 1u << 3,
        Place = 1u << 4, Fire = 1u << 5, Next = 1u << 6, Prev = 1u << 7,
    }

    // A plugin call result / prop value: 'n' number, 'b' flag, 't' text, 'x' failed.
    public struct Value
    {
        public char Type;
        public double Number;
        public string Text;
        public static implicit operator Value(double v) => new Value { Type = 'n', Number = v, Text = "" };
        public static implicit operator Value(bool v) => new Value { Type = 'b', Number = v ? 1 : 0, Text = "" };
        public static implicit operator Value(string v) => new Value { Type = 't', Text = v ?? "" };
        public override string ToString() => Type == 't' ? Text : Type == 'b' ? (Number != 0 ? "true" : "false")
            : Number.ToString(CultureInfo.InvariantCulture);
    }

    // Right-handed, Y up. Forward is the entity's -Z. Fields are settable
    // (`velocity.Y = -2f`). ToString is readable in logs.
    public struct Vector3
    {
        public float X, Y, Z;
        public Vector3(float x, float y, float z) { X = x; Y = y; Z = z; }
        public static Vector3 Zero => new Vector3(0, 0, 0);
        public static Vector3 One => new Vector3(1, 1, 1);
        public static Vector3 Up => new Vector3(0, 1, 0);
        public static Vector3 Down => new Vector3(0, -1, 0);
        public static Vector3 Right => new Vector3(1, 0, 0);
        public static Vector3 Forward => new Vector3(0, 0, -1);
        public static Vector3 operator +(Vector3 a, Vector3 b) => new Vector3(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static Vector3 operator -(Vector3 a, Vector3 b) => new Vector3(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public static Vector3 operator *(Vector3 a, float s) => new Vector3(a.X * s, a.Y * s, a.Z * s);
        public static Vector3 operator *(float s, Vector3 a) => a * s;
        public float Magnitude => (float)Math.Sqrt(X * X + Y * Y + Z * Z);
        public override string ToString() =>
            "(" + X.ToString("0.###", CultureInfo.InvariantCulture) + ", " +
            Y.ToString("0.###", CultureInfo.InvariantCulture) + ", " +
            Z.ToString("0.###", CultureInfo.InvariantCulture) + ")";
    }

    public struct Vector2
    {
        public float X, Y;
        public Vector2(float x, float y) { X = x; Y = y; }
        public static Vector2 Zero => new Vector2(0, 0);
        public static Vector2 operator +(Vector2 a, Vector2 b) => new Vector2(a.X + b.X, a.Y + b.Y);
        public static Vector2 operator *(Vector2 a, float s) => new Vector2(a.X * s, a.Y * s);
        public override string ToString() =>
            "(" + X.ToString("0.###", CultureInfo.InvariantCulture) + ", " +
            Y.ToString("0.###", CultureInfo.InvariantCulture) + ")";
    }

    // A live object. Null when Find misses. Implicitly a ulong id so older
    // calls (`Get("gold", -1, Find("Bank"))`) keep compiling.
    public sealed class Entity
    {
        internal Script Owner;
        public ulong Id;
        public static implicit operator ulong(Entity e) => e == null ? 0UL : e.Id;
        public static bool operator ==(Entity a, Entity b)
        {
            if (ReferenceEquals(a, b)) return true;
            if (a is null || b is null) return false;
            return a.Id == b.Id;
        }
        public static bool operator !=(Entity a, Entity b) => !(a == b);
        public override bool Equals(object obj) => obj is Entity e && this == e;
        public override int GetHashCode() => Id.GetHashCode();
        public Vector3 Position
        {
            get => Owner == null ? Vector3.Zero : Owner.PosePosition(Id);
            set { if (Owner != null) Owner.SetPosition(Id, value); }
        }
        public void SetLocalRotation(float x, float y, float z)
        {
            if (Owner != null) Owner.SetLocalEuler(Id, x, y, z);
        }
        public void SetVisible(bool on) { if (Owner != null) Owner.SetVisible(Id, on); }
        public void Destroy() { if (Owner != null) Owner.Destroy(Id); }
    }

    // Axes of one object. Script.Transform returns this for the script's own body.
    public sealed class Transform
    {
        internal Script Owner;
        internal ulong Id;
        public Vector3 Position
        {
            get => Owner.PosePosition(Id);
            set => Owner.SetPosition(Id, value);
        }
        public Vector3 Forward => Owner.PoseAxis(Id, 9);
        public Vector3 Right => Owner.PoseAxis(Id, 12);
        public Vector3 Up => Owner.PoseAxis(Id, 15);
        public void Rotate(Vector3 eulerDegrees) => Owner.RotateBy(Id, eulerDegrees);
        public void SetLocalRotation(float x, float y, float z) => Owner.SetLocalEuler(Id, x, y, z);
    }

    public static class Input
    {
        public static float GetAxis(string name) => Script.Active == null ? 0f : Script.Active.Axis(name);
        public static bool GetButtonDown(string name) => Script.Active != null && Script.Active.ButtonDown(name);
        public static bool GetButton(string name) => Script.Active != null && Script.Active.ButtonHeld(name);
    }

    public static class Time
    {
        public static float DeltaTime { get; internal set; }
        public static float Elapsed { get; internal set; }
        public static int Frame { get; internal set; }
    }

    public abstract unsafe class Script
    {
        internal TsScriptApi* api;
        internal Dictionary<string, Value> props = new Dictionary<string, Value>();
        internal static Script Active;
        public ulong Self { get; internal set; }

        // Hooks. OnTick's default runs OnUpdate, so a script overrides one of them.
        // A script that overrides OnTick and does not call the base skips OnUpdate.
        public virtual void OnStart() { }
        public virtual void OnEnable() { }
        public virtual void OnDisable() { }
        public virtual void OnUpdate(float deltaTime) { }
        public virtual void OnFixedUpdate(float deltaTime) { }
        public virtual void OnLateUpdate(float deltaTime) { }
        public virtual void OnTick(double dt) { OnUpdate((float)dt); }
        public virtual void OnTriggerEnter(ulong other) { }
        public virtual void OnTriggerExit(ulong other) { }
        public virtual void OnInteract(ulong rider) { }
        public virtual void OnReload() { }
        public virtual void OnDestroy() { }
        public virtual void OnHand(string hook, string hand, double n) { }
        public virtual void OnUi(string widget, bool change, double value, string text) { }

        // Plugin events (ts.quests / ts.dialogue / ts.saves, ...). The default
        // OnEvent routes the story hooks to the virtuals below; override it to
        // see every event.
        public virtual void OnEvent(string hook, string text, double n)
        {
            switch (hook)
            {
                case "on_quest_started": OnQuestStarted(text); break;
                case "on_quest_completed": OnQuestCompleted(text); break;
                case "on_quest_failed": OnQuestFailed(text); break;
                case "on_objective": OnObjective(text, n); break;
                case "on_dialogue_event": OnDialogueEvent(text, n); break;
                case "on_dialogue_end": OnDialogueEnd(text); break;
                case "on_game_saved": OnGameSaved(text); break;
                case "on_game_loaded": OnGameLoaded(text); break;
            }
        }
        public virtual void OnQuestStarted(string id) { }
        public virtual void OnQuestCompleted(string id) { }
        public virtual void OnQuestFailed(string id) { }
        public virtual void OnObjective(string questSlashObjective, double count) { }
        public virtual void OnDialogueEvent(string name, double n) { }
        public virtual void OnDialogueEnd(string dialogueId) { }
        public virtual void OnGameSaved(string slot) { }
        public virtual void OnGameLoaded(string slot) { }
        // Hot reload keeps fields (float, int, bool, string, Vector2, Vector3)
        // unless a script replaces Save / Load. Prefixed so a hand-written
        // save string is left alone.
        public virtual string Save() => PackFields();
        public virtual void Load(string state) { if (state != null && state.StartsWith("tsf1\n")) UnpackFields(state); }

        IntPtr W => api->World;
        ulong Id(ulong e) => e != 0 ? e : Self;

        // The world.
        public ulong Tick => api->Tick(W);
        public Entity Find(string name)
        {
            ulong id;
            fixed (byte* p = Utf8.Z(name)) id = api->Find(W, p);
            return id == 0 ? null : new Entity { Owner = this, Id = id };
        }
        public Entity Player => FindPlayer(1);
        Entity FindPlayer(int slot1)
        {
            ulong id = api->Player(W, slot1 - 1);
            return id == 0 ? null : new Entity { Owner = this, Id = id };
        }
        // Slot is 1-based, as before. The Player property is slot 1.
        public ulong PlayerId(int slot1) => api->Player(W, slot1 - 1);
        public ulong Player(int slot1) => api->Player(W, slot1 - 1);
        public bool Position(ulong e, out float x, out float y)
        {
            float a = 0, b = 0;
            bool ok = api->Position(W, e, &a, &b) != 0;
            x = a; y = b;
            return ok;
        }
        public Vector3 Position
        {
            get => PosePosition(Self);
            set => SetPosition(Self, value);
        }
        public Transform Transform => new Transform { Owner = this, Id = Self };
        public void Move(Vector3 delta) => MoveBy(Self, delta);
        public void Rotate(Vector3 eulerDegrees) => RotateBy(Self, eulerDegrees);
        public void Move(ulong e, float x, float y) => api->Move(W, e, x, y);
        public void SetVisible(ulong e, bool on) => api->SetVisible(W, e, on ? 1 : 0);
        public void SetCollider(ulong e, bool on) => api->SetCollider(W, e, on ? 1 : 0);
        public bool Play(ulong e, string clip, bool hold = false) { fixed (byte* p = Utf8.Z(clip)) return api->Play(W, e, p, hold ? 1 : 0) != 0; }
        public ulong Spawn(string tmpl, float x, float y) { fixed (byte* p = Utf8.Z(tmpl)) return api->Spawn(W, p, x, y); }
        public void Destroy(ulong e) => api->Destroy(W, e);
        // Entity state (shared with Lua / graphs / C++, replicated).
        public double Get(string key, double fallback = 0, ulong e = 0)
        {
            double v = fallback;
            fixed (byte* p = Utf8.Z(key)) return api->GetNumber(W, Id(e), p, &v) != 0 ? v : fallback;
        }
        public string GetText(string key, ulong e = 0)
        {
            byte* buf = stackalloc byte[1024];
            int n;
            fixed (byte* p = Utf8.Z(key)) n = api->GetText(W, Id(e), p, buf, 1024);
            return n < 0 ? "" : Utf8.Read(buf, Math.Min(n, 1023));
        }
        public void Set(string key, double v, ulong e = 0) { fixed (byte* p = Utf8.Z(key)) api->SetNumber(W, Id(e), p, v); }
        public void Set(string key, bool v, ulong e = 0) => Set(key, v ? 1.0 : 0.0, e);
        public void Set(string key, string v, ulong e = 0)
        {
            fixed (byte* k = Utf8.Z(key)) fixed (byte* t = Utf8.Z(v)) api->SetText(W, Id(e), k, t);
        }
        public void Toast(string text, int slot1 = 0, double seconds = 2.0)
        {
            fixed (byte* p = Utf8.Z(text)) api->Toast(W, slot1 <= 0 ? -1 : slot1 - 1, p, seconds, Self);
        }
        public void Toast(string title, string message)
        {
            string text = string.IsNullOrEmpty(message) ? (title ?? "") : (title ?? "") + " — " + message;
            Toast(text);
        }
        void LogAt(int level, string text, string file, int line)
        {
            if (api->LogAt != null)
            {
                fixed (byte* p = Utf8.Z(text)) fixed (byte* f = Utf8.Z(file ?? ""))
                    api->LogAt(W, level, p, Self, f, line);
                return;
            }
            fixed (byte* p = Utf8.Z(text)) api->Log(W, level, p, Self);
        }
        public void Log(string text, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) => LogAt(0, text, file, line);
        public void Warn(string text, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) => LogAt(1, text, file, line);
        public void ErrorLog(string text, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) => LogAt(2, text, file, line);
        internal void Error(string text) => LogAt(2, text, "", 0);
        public void Sound(string clip, float volume = 1) { fixed (byte* p = Utf8.Z(clip)) api->Sound(W, p, volume, Self); }
        public void Music(string clip, float fade = 1) { fixed (byte* p = Utf8.Z(clip)) api->Music(W, p, fade); }
        public void Burst(string preset, ulong at = 0) { fixed (byte* p = Utf8.Z(preset)) api->FxBurst(W, p, Id(at)); }
        public void UiShow(string doc) => Ui(0, doc, "", 0);
        public void UiHide(string doc) => Ui(1, doc, "", 0);
        public void UiText(string id, string text) => Ui(2, id, text, 0);
        void Ui(int kind, string target, string text, double v)
        {
            fixed (byte* a = Utf8.Z(target)) fixed (byte* b = Utf8.Z(text)) api->Ui(W, kind, a, b, v);
        }
        public bool Held(Button b, int slot1 = 1) => (api->Buttons(W, slot1 - 1) & (uint)b) != 0;

        // Localisation: Tr("menu.start"), Tr("gold.count", "12") fills {0}.
        public string Tr(string key, params string[] args)
        {
            if (api->Translate == null) return key;
            string enc = args.Length == 0 ? "" : string.Join("\n", args) + "\n";
            byte[] buf = new byte[256];
            int n;
            fixed (byte* k = Utf8.Z(key)) fixed (byte* a = Utf8.Z(enc))
            {
                fixed (byte* bp = buf) n = api->Translate(W, k, a, bp, buf.Length);
                if (n > buf.Length)
                {
                    buf = new byte[n];
                    fixed (byte* bp = buf) n = api->Translate(W, k, a, bp, buf.Length);
                }
            }
            return n <= 0 ? "" : System.Text.Encoding.UTF8.GetString(buf, 0, n);
        }
        public bool SetLanguage(string lang) { fixed (byte* p = Utf8.Z(lang)) return api->LangSet != null && api->LangSet(W, p) != 0; }
        public string Language
        {
            get
            {
                byte* b = stackalloc byte[64];
                int n = api->LangGet == null ? 0 : api->LangGet(W, b, 64);
                return Utf8.Read(b, Math.Min(n, 64));
            }
        }

        // Plugin call bridge: table.fn(args...) on a plugin's Lua table.
        // .Type 'x' when the call failed (the error is in the Telegraph).
        public Value Plugin(string table, string fn, params Value[] args)
        {
            var enc = new System.Text.StringBuilder();
            foreach (Value a in args)
            {
                enc.Append(a.Type).Append('\t');
                enc.Append(a.Type == 't' ? a.Text : a.Type == 'b' ? (a.Number != 0 ? "1" : "0")
                           : a.Number.ToString("R", CultureInfo.InvariantCulture));
                enc.Append('\n');
            }
            var r = new Value { Type = 'x', Text = "" };
            if (api->PluginCall == null) return r;
            byte[] buf = new byte[256];
            int n;
            fixed (byte* t = Utf8.Z(table)) fixed (byte* f = Utf8.Z(fn)) fixed (byte* a = Utf8.Z(enc.ToString()))
            {
                fixed (byte* bp = buf) n = api->PluginCall(W, t, f, a, bp, buf.Length);
                if (n > buf.Length)
                {
                    buf = new byte[n];
                    fixed (byte* bp = buf) n = api->PluginCall(W, t, f, a, bp, buf.Length);
                }
            }
            if (n < 2) return r;
            string res = System.Text.Encoding.UTF8.GetString(buf, 0, n);
            r.Type = res[0];
            r.Text = res.Substring(2);
            if (r.Type == 'n') double.TryParse(r.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out r.Number);
            if (r.Type == 'b') r.Number = r.Text == "1" ? 1 : 0;
            return r;
        }

        // Props (scene overrides of the defaults you pass). The overload
        // matches the default's type, so `float s = Prop("moveSpeed", 6.0f)` compiles.
        public float Prop(string name, float fallback) => (float)Prop(name, (double)fallback);
        public int Prop(string name, int fallback) => (int)Prop(name, (double)fallback);
        public double Prop(string name, double fallback) => props.TryGetValue(name, out Value v) ? v.Number : fallback;
        public string Prop(string name, string fallback) => props.TryGetValue(name, out Value v) ? (v.Type == 't' ? v.Text : v.ToString()) : fallback;
        public bool Prop(string name, bool fallback) => props.TryGetValue(name, out Value v) ? v.Number != 0 : fallback;
        public Vector3 Prop(string name, Vector3 fallback)
        {
            if (!props.TryGetValue(name, out Value v) || v.Type != 't') return fallback;
            string[] p = (v.Text ?? "").Split(',');
            if (p.Length < 3) return fallback;
            float x, y, z;
            if (!float.TryParse(p[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x)) return fallback;
            if (!float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out y)) return fallback;
            if (!float.TryParse(p[2], NumberStyles.Float, CultureInfo.InvariantCulture, out z)) return fallback;
            return new Vector3(x, y, z);
        }

        internal bool ReadPose(ulong id, float[] o)
        {
            if (o == null || o.Length < 18 || api->Pose == null) return false;
            float* buf = stackalloc float[18];
            if (api->Pose(W, id, buf) == 0) return false;
            for (int i = 0; i < 18; ++i) o[i] = buf[i];
            return true;
        }
        internal Vector3 PosePosition(ulong id)
        {
            float[] o = new float[18];
            if (!ReadPose(id, o))
            {
                float x, y;
                if (Position(id, out x, out y)) return new Vector3(x, y, 0);
                return Vector3.Zero;
            }
            return new Vector3(o[0], o[1], o[2]);
        }
        internal Vector3 PoseAxis(ulong id, int at)
        {
            float[] o = new float[18];
            if (!ReadPose(id, o)) return Vector3.Zero;
            return new Vector3(o[at], o[at + 1], o[at + 2]);
        }
        internal void MoveBy(ulong id, Vector3 d)
        {
            if (api->MoveBy != null) api->MoveBy(W, id, d.X, d.Y, d.Z);
        }
        internal void RotateBy(ulong id, Vector3 e)
        {
            if (api->RotateBy != null) api->RotateBy(W, id, e.X, e.Y, e.Z);
        }
        internal void SetLocalEuler(ulong id, float x, float y, float z)
        {
            if (api->SetLocalEuler != null) api->SetLocalEuler(W, id, x, y, z);
        }
        internal void SetPosition(ulong id, Vector3 p)
        {
            if (api->SetPosition != null) api->SetPosition(W, id, p.X, p.Y, p.Z);
            else api->Move(W, id, p.X, p.Y);
        }
        internal float Axis(string name)
        {
            if (api->Axis == null) return 0;
            fixed (byte* p = Utf8.Z(name)) return api->Axis(W, 0, p);
        }
        internal bool ButtonDown(string name)
        {
            if (api->ButtonDown == null) return false;
            fixed (byte* p = Utf8.Z(name)) return api->ButtonDown(W, 0, p) != 0;
        }
        internal bool ButtonHeld(string name)
        {
            if (name == "Jump") return Held(Button.Action);
            return false;
        }

        string PackFields()
        {
            var sb = new StringBuilder("tsf1\n");
            for (Type t = GetType(); t != null && t != typeof(Script); t = t.BaseType)
            {
                foreach (FieldInfo f in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                {
                    if (f.IsStatic || f.Name.IndexOf('<') >= 0) continue;
                    object v = f.GetValue(this);
                    if (f.FieldType == typeof(float)) sb.Append(f.Name).Append("\tf\t").Append(((float)v).ToString("R", CultureInfo.InvariantCulture)).Append('\n');
                    else if (f.FieldType == typeof(double)) sb.Append(f.Name).Append("\td\t").Append(((double)v).ToString("R", CultureInfo.InvariantCulture)).Append('\n');
                    else if (f.FieldType == typeof(int)) sb.Append(f.Name).Append("\ti\t").Append((int)v).Append('\n');
                    else if (f.FieldType == typeof(bool)) sb.Append(f.Name).Append("\tb\t").Append((bool)v ? "1" : "0").Append('\n');
                    else if (f.FieldType == typeof(string)) sb.Append(f.Name).Append("\ts\t").Append(((string)v) ?? "").Append('\n');
                    else if (f.FieldType == typeof(Vector3))
                    {
                        Vector3 p = (Vector3)v;
                        sb.Append(f.Name).Append("\tv\t").Append(p.X.ToString("R", CultureInfo.InvariantCulture)).Append(',')
                          .Append(p.Y.ToString("R", CultureInfo.InvariantCulture)).Append(',')
                          .Append(p.Z.ToString("R", CultureInfo.InvariantCulture)).Append('\n');
                    }
                    else if (f.FieldType == typeof(Vector2))
                    {
                        Vector2 p = (Vector2)v;
                        sb.Append(f.Name).Append("\t2\t").Append(p.X.ToString("R", CultureInfo.InvariantCulture)).Append(',')
                          .Append(p.Y.ToString("R", CultureInfo.InvariantCulture)).Append('\n');
                    }
                }
            }
            return sb.ToString();
        }
        void UnpackFields(string state)
        {
            foreach (string line in state.Split('\n'))
            {
                if (line.Length < 4 || line == "tsf1") continue;
                string[] f = line.Split('\t');
                if (f.Length < 3) continue;
                FieldInfo field = null;
                for (Type t = GetType(); t != null && t != typeof(Script) && field == null; t = t.BaseType)
                    field = t.GetField(f[0], BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
                if (field == null) continue;
                if (f[1] == "f" && field.FieldType == typeof(float))
                { float n; if (float.TryParse(f[2], NumberStyles.Float, CultureInfo.InvariantCulture, out n)) field.SetValue(this, n); }
                else if (f[1] == "d" && field.FieldType == typeof(double))
                { double n; if (double.TryParse(f[2], NumberStyles.Float, CultureInfo.InvariantCulture, out n)) field.SetValue(this, n); }
                else if (f[1] == "i" && field.FieldType == typeof(int))
                { int n; if (int.TryParse(f[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out n)) field.SetValue(this, n); }
                else if (f[1] == "b" && field.FieldType == typeof(bool)) field.SetValue(this, f[2] == "1");
                else if (f[1] == "s" && field.FieldType == typeof(string)) field.SetValue(this, f[2]);
                else if (f[1] == "v" && field.FieldType == typeof(Vector3)) field.SetValue(this, ParseVec(f[2]));
                else if (f[1] == "2" && field.FieldType == typeof(Vector2))
                {
                    string[] p = f[2].Split(',');
                    float x, y;
                    if (p.Length >= 2 && float.TryParse(p[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x)
                        && float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out y))
                        field.SetValue(this, new Vector2(x, y));
                }
            }
        }
        static Vector3 ParseVec(string text)
        {
            string[] p = (text ?? "").Split(',');
            float x = 0, y = 0, z = 0;
            if (p.Length >= 3)
            {
                float.TryParse(p[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x);
                float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out y);
                float.TryParse(p[2], NumberStyles.Float, CultureInfo.InvariantCulture, out z);
            }
            return new Vector3(x, y, z);
        }
    }
}
