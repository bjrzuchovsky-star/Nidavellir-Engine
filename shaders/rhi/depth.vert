#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 2) in vec2 a_uv;
#ifdef TS_SKIN
layout(location = 4) in vec4 a_joints;
layout(location = 5) in vec4 a_weights;
#endif
#ifdef TS_FOLIAGE
layout(location = 6) in vec4 a_inst;
layout(location = 7) in vec2 a_inst2;
#endif

mat4 ts_skin_matrix() {
#ifdef TS_SKIN
  return a_weights.x * bones.u_bones[int(a_joints.x)] +
         a_weights.y * bones.u_bones[int(a_joints.y)] +
         a_weights.z * bones.u_bones[int(a_joints.z)] +
         a_weights.w * bones.u_bones[int(a_joints.w)];
#else
  return mat4(1.0);
#endif
}

float ts_wind_bend(vec2 at) {
  float ph = frame.timev.x * (1.3 + 0.7 * frame.wind.z) + at.x * 0.13 + at.y * 0.11;
  return frame.wind.z * (0.6 + 0.4 * sin(ph)) + frame.wind.w * 0.5 * sin(ph * 2.7 + at.x * 0.3);
}

void main() {
  vec3 world;
#ifdef TS_FOLIAGE
  float c = cos(a_inst.w), s = sin(a_inst.w);
  vec3 p = a_pos * a_inst2.x;
  vec3 r = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
  float weight = draw.foliage_a.w > 0.0 ? clamp(a_pos.y / draw.foliage_a.w, 0.0, 1.0) : a_uv.y;
  float k = ts_wind_bend(a_inst.xz) * draw.extra.w * weight * weight * 0.35 * a_inst2.x;
  world = a_inst.xyz + r + vec3(frame.wind.x * k, -abs(k) * 0.15, frame.wind.y * k);
  if (a_inst2.y < 0.5) world = vec3(0.0, -1.0e6, 0.0);
#else
  world = (draw.model * ts_skin_matrix() * vec4(a_pos, 1.0)).xyz;
#endif
  int cascade = int(draw.extra.x + 0.5);
  mat4 lp = frame.light_vp[clamp(cascade, 0, 3)];
  gl_Position = ts_clip(lp * vec4(world, 1.0));
}
