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

    public abstract unsafe class Script
    {
        internal TsScriptApi* api;
        internal Dictionary<string, Value> props = new Dictionary<string, Value>();
        public ulong Self { get; internal set; }

        // Hooks.
        public virtual void OnStart() { }
        public virtual void OnTick(double dt) { }
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
        public virtual string Save() => "";
        public virtual void Load(string state) { }

        IntPtr W => api->World;
        ulong Id(ulong e) => e != 0 ? e : Self;

        // The world.
        public ulong Tick => api->Tick(W);
        public ulong Find(string name) { fixed (byte* p = Utf8.Z(name)) return api->Find(W, p); }
        public ulong Player(int slot1) => api->Player(W, slot1 - 1);
        public bool Position(ulong e, out float x, out float y)
        {
            float a = 0, b = 0;
            bool ok = api->Position(W, e, &a, &b) != 0;
            x = a; y = b;
            return ok;
        }
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
        void LogAt(int level, string text) { fixed (byte* p = Utf8.Z(text)) api->Log(W, level, p, Self); }
        public void Log(string text) => LogAt(0, text);
        public void Warn(string text) => LogAt(1, text);
        internal void Error(string text) => LogAt(2, text);
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

        // Props (scene overrides of the defaults you pass).
        public double Prop(string name, double fallback) => props.TryGetValue(name, out Value v) ? v.Number : fallback;
        public string Prop(string name, string fallback) => props.TryGetValue(name, out Value v) ? v.Text : fallback;
        public bool Prop(string name, bool fallback) => props.TryGetValue(name, out Value v) ? v.Number != 0 : fallback;
    }
}
