#pragma once

// --smoke-mesh-parts: split a multi-part model into assets, place them as
// a parent with children (undo / redo), and check mesh-fitted colliders
// (oriented box, simplified convex hull, convex pieces, triangle mesh,
// sprite polygon) including a real Jolt / Box2D contact.
int run_mesh_parts_smoke();
