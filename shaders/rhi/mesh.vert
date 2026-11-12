#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_tan;
#ifdef TS_SKIN
layout(location = 4) in vec4 a_joints;
layout(location = 5) in vec4 a_weights;
#endif
#ifdef TS_FOLIAGE
layout(location = 6) in vec4 a_inst;
layout(location = 7) in vec2 a_inst2;
#endif

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_nrm;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tan;
layout(location = 4) out vec4 v_light0;
layout(location = 5) out vec4 v_light1;
layout(location = 6) out vec4 v_light2;
layout(location = 7) out vec4 v_light3;
layout(location = 8) out float v_view_z;
layout(location = 9) out float v_part;
layout(location = 10) out float v_fade;

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
#ifdef TS_FOLIAGE
  float c = cos(a_inst.w), s = sin(a_inst.w);
  vec3 p = a_pos * a_inst2.x;
  vec3 r = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
  vec3 nrm = vec3(c * a_nrm.x + s * a_nrm.z, a_nrm.y, -s * a_nrm.x + c * a_nrm.z);
  float weight = draw.foliage_a.w > 0.0 ? clamp(a_pos.y / draw.foliage_a.w, 0.0, 1.0) : a_uv.y;
  float k = ts_wind_bend(a_inst.xz) * draw.extra.w * weight * weight * 0.35 * a_inst2.x;
  vec3 world = a_inst.xyz + r + vec3(frame.wind.x * k, -abs(k) * 0.15, frame.wind.y * k);
  v_world = world;
  v_nrm = nrm;
  v_uv = a_uv;
  v_tan = vec4(1.0, 0.0, 0.0, 1.0);
  v_part = a_uv.x;
  v_fade = a_inst2.y;
  vec4 w = vec4(world, 1.0);
#else
  mat4 model = draw.model * ts_skin_matrix();
  vec4 w = model * vec4(a_pos, 1.0);
  mat3 nm = transpose(inverse(mat3(model)));
  v_world = w.xyz;
  v_nrm = normalize(nm * a_nrm);
  v_tan = vec4(normalize(mat3(model) * a_tan.xyz), a_tan.w);
  v_uv = a_uv;
  v_part = a_uv.x;
  v_fade = 1.0;
#endif
  v_light0 = frame.light_vp[0] * w;
  v_light1 = frame.light_vp[1] * w;
  v_light2 = frame.light_vp[2] * w;
  v_light3 = frame.light_vp[3] * w;
  v_view_z = length(frame.cam.xyz - w.xyz);
  gl_Position = ts_clip(frame.view_proj * w);
}
