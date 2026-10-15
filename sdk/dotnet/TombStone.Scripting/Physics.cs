// Physics from C# scripts: wrappers over the plugin bridge (the same
// `physics` Lua table scripts and graphs use). 2D worlds: px, px / s, degrees
// (z ignored); 3D worlds: metres, m / s, y up.
//
//   public override void OnInteract(ulong rider) { this.ApplyImpulse(Self, 0, -400); }

namespace TombStone
{
    public static class Physics
    {
        static bool Ok(Value v) => v.Type != 'x' && v.Type != 't' && v.Number != 0;
        static double Num(Value v) => v.Type == 'n' || v.Type == 'b' ? v.Number : 0;

        public static bool ApplyForce(this Script s, ulong e, double x, double y, double z = 0) =>
            Ok(s.Plugin("physics", "apply_force", (double)e, x, y, z));
        public static bool ApplyImpulse(this Script s, ulong e, double x, double y, double z = 0) =>
            Ok(s.Plugin("physics", "apply_impulse", (double)e, x, y, z));
        public static bool ApplyTorque(this Script s, ulong e, double x, double y = 0, double z = 0) =>
            Ok(s.Plugin("physics", "apply_torque", (double)e, x, y, z));
        public static bool SetVelocity(this Script s, ulong e, double x, double y, double z = 0) =>
            Ok(s.Plugin("physics", "set_velocity", (double)e, x, y, z));
        // axis 0 = x, 1 = y, 2 = z
        public static double Velocity(this Script s, ulong e, int axis) =>
            Num(s.Plugin("physics", "velocity_axis", (double)e, (double)axis));
        public static double Speed(this Script s, ulong e) => Num(s.Plugin("physics", "speed", (double)e));
        // First entity the ray hits (0 = none). 2D: z = dz = 0.
        public static ulong Raycast(this Script s, double x, double y, double z, double dx, double dy, double dz, double dist)
        {
            Value v = s.Plugin("physics", "raycast_xyz", x, y, z, dx, dy, dz, dist);
            return v.Type == 'n' && v.Number > 0 ? (ulong)v.Number : 0;
        }
        public static double RayDistance(this Script s, double x, double y, double z, double dx, double dy, double dz, double dist)
        {
            Value v = s.Plugin("physics", "ray_distance_xyz", x, y, z, dx, dy, dz, dist);
            return v.Type == 'n' ? v.Number : -1;
        }
        // Join b to a (0 = the world); anchor relative to b. Joint handle, 0 = failed.
        public static double Joint(this Script s, ulong a, ulong b, string type, double ax = 0, double ay = 0, double az = 0) =>
            Num(s.Plugin("physics", "joint_xyz", (double)a, (double)b, type, ax, ay, az));
        public static void Unjoint(this Script s, double joint) => s.Plugin("physics", "unjoint", joint);
    }
}
