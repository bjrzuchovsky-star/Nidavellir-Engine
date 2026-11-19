#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#include "common.glsl"
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

#ifdef TS_RAY
layout(set = 0, binding = 15) uniform accelerationStructureEXT u_tlas;
#endif

void main() {
  vec3 dir = vec3(cos(v_uv.x * 6.2831853) * cos((v_uv.y - 0.5) * 3.14159265),
                  sin((v_uv.y - 0.5) * 3.14159265),
                  sin(v_uv.x * 6.2831853) * cos((v_uv.y - 0.5) * 3.14159265));
  vec3 sky = mix(frame.sky_horizon.rgb, frame.sky_top.rgb, clamp(dir.y * 0.5 + 0.5, 0.0, 1.0));
#ifdef TS_RAY
  rayQueryEXT q;
  rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, frame.cam.xyz, 0.05, dir, 80.0);
  while (rayQueryProceedEXT(q)) {}
  if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) sky *= 0.18;
#endif
  o_color = vec4(sky, 1.0);
}
