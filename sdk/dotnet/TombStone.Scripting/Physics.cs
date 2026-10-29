// Physics from C# scripts: wrappers over the plugin bridge (the same
// `physics` Lua table scripts and graphs use). 2D worlds: px, px / s, degrees
// (z ignored); 3D worlds: metres, m / s, y up.
//
//   public override void OnInteract(ulong rider) { this.ApplyImpulse(Self, 0, -400); }

using System.Globalization;

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

        // Built-in queries (no physics plugin required). Raycast hits solid colliders.
        public struct RayHit
        {
            public ulong Entity;
            public Vector3 Point;
            public float Distance;
        }
        public static bool Raycast(Vector3 origin, Vector3 direction, out RayHit hit, float maxDistance = 100f)
        {
            hit = new RayHit();
            Script s = Script.Active;
            if (s == null) return false;
            string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);
            string args = F(origin.X) + " " + F(origin.Y) + " " + F(origin.Z) + " " + F(direction.X) + " " +
                          F(direction.Y) + " " + F(direction.Z) + " " + F(maxDistance);
            string res = s.EngineQuery("raycast", args);
            if (string.IsNullOrEmpty(res) || res == "0") return false;
            string[] p = res.Split(' ');
            ulong id;
            float dist, x, y, z;
            if (p.Length < 5 || !ulong.TryParse(p[0], out id) || id == 0) return false;
            float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out dist);
            float.TryParse(p[2], NumberStyles.Float, CultureInfo.InvariantCulture, out x);
            float.TryParse(p[3], NumberStyles.Float, CultureInfo.InvariantCulture, out y);
            float.TryParse(p[4], NumberStyles.Float, CultureInfo.InvariantCulture, out z);
            hit.Entity = id;
            hit.Distance = dist;
            hit.Point = new Vector3(x, y, z);
            return true;
        }
        // First solid collider within radius of the point. 0 when nothing is there.
        public static ulong Overlap(Vector3 point, float radius)
        {
            Script s = Script.Active;
            if (s == null) return 0;
            string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);
            string args = F(point.X) + " " + F(point.Y) + " " + F(point.Z) + " " + F(radius);
            string res = s.EngineQuery("overlap", args);
            ulong id;
            return ulong.TryParse(res, out id) ? id : 0;
        }
    }
}
