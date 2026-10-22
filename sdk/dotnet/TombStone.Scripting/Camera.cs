// Cameras from C# scripts: wrappers over the plugin bridge (the same
// `camera` Lua table scripts and graphs use). See docs/camera.md.
//
//   public override void OnInteract(ulong rider) { this.SetActiveCamera("Cellar Camera"); }

namespace TombStone
{
    public static class Camera
    {
        static bool Ok(Value v) => v.Type != 'x' && v.Type != 't' && v.Number != 0;
        static double Num(Value v) => v.Type == 'n' || v.Type == 'b' ? v.Number : 0;

        // Look through this camera now (until reset or another SetActiveCamera).
        public static bool SetActiveCamera(this Script s, ulong camera) =>
            Ok(s.Plugin("camera", "set_active", (double)camera));
        public static bool SetActiveCamera(this Script s, string name) =>
            Ok(s.Plugin("camera", "set_active", name));
        // Back to the highest-priority active camera.
        public static void ResetCamera(this Script s) => s.Plugin("camera", "reset");
        // Camera Play looks through (0 = the built-in follow view).
        public static ulong ActiveCamera(this Script s) => (ulong)Num(s.Plugin("camera", "active"));
        public static bool SetCameraPriority(this Script s, ulong camera, int priority) =>
            Ok(s.Plugin("camera", "set_priority", (double)camera, (double)priority));
    }
}
