// ts.ai from C# scripts: wrappers over the plugin bridge (the same `ai` Lua
// table scripts and graphs use). Needs the ts.ai plugin on.
//
//   public override void OnStart() { this.AiAgent(Self, 120); this.AiTree(Self, "ai/guard.bt.json"); }
//
// 2D positions are world px (x, y); 3D ones metres (x, y up, z): use the *3 forms.

namespace TombStone
{
    public static class Ai
    {
        static bool Ok(Value v) => v.Type != 'x' && v.Number != 0;

        public static bool AiAgent(this Script s, ulong e, double speed = 0) => Ok(s.Plugin("ai", "agent", (double)e, speed));
        public static bool AiTree(this Script s, ulong e, string path) => Ok(s.Plugin("ai", "bt", (double)e, path));
        public static bool AiMoveTo(this Script s, ulong e, double x, double y) => Ok(s.Plugin("ai", "move_to", (double)e, x, y));
        public static bool AiMoveTo3(this Script s, ulong e, double x, double y, double z) =>
            Ok(s.Plugin("ai", "move_to", (double)e, x, y, z));
        public static void AiStop(this Script s, ulong e) => s.Plugin("ai", "stop", (double)e);
        public static bool AiMoving(this Script s, ulong e) => Ok(s.Plugin("ai", "moving", (double)e));
        // Walkable path length, -1 when there is none.
        public static double AiPathLength(this Script s, double x0, double y0, double x1, double y1)
        {
            Value v = s.Plugin("ai", "path_length", x0, y0, x1, y1);
            return v.Type == 'x' ? -1 : v.Number;
        }
        public static double AiPathLength3(this Script s, double x0, double y0, double z0, double x1, double y1, double z1)
        {
            Value v = s.Plugin("ai", "path_length", x0, y0, z0, x1, y1, z1);
            return v.Type == 'x' ? -1 : v.Number;
        }
        public static string AiState(this Script s, ulong e) => s.Plugin("ai", "state", (double)e).Text ?? "";
        public static void AiSetBb(this Script s, ulong e, string key, Value v) => s.Plugin("ai", "set_bb", (double)e, key, v);
        public static double AiGetBb(this Script s, ulong e, string key)
        {
            Value v = s.Plugin("ai", "get_bb", (double)e, key);
            return v.Type == 'x' ? 0 : v.Number;
        }
        public static void AiNoise(this Script s, double x, double y, double loudness = 1) => s.Plugin("ai", "noise", x, y, loudness);
        public static bool AiCanSee(this Script s, ulong e, ulong target) => Ok(s.Plugin("ai", "can_see", (double)e, (double)target));
        public static int AiCount(this Script s) => (int)s.Plugin("ai", "count").Number;
    }
}
