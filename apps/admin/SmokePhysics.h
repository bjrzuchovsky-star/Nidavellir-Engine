#pragma once

// Physics: the Jolt-backed 3D core (stacking stability, joint limits for all
// five kinds, ray / shape casts, sensors, contact impulses, compound /
// convex / mesh shapes, the character controller, a determinism hash),
// World3D on it (rigid bodies, VR held objects that push and knock things
// over, throws with the hand's velocity, desktop physics-gun) and the
// Box2D-backed 2D bodies (rotated colliders, joints, determinism).
int run_physics_smoke();
