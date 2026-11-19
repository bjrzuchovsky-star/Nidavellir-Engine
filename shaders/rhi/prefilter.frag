#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// One face of one roughness level. ibl.z is the level 0..3, ibl.w the face 0..5,
// ibl.y the source face size in the sharp atlas (uv of one face is 1/6 by 1).
void main() {
  float level = frame.ibl.z;
  float face = frame.ibl.w;
  float rough = level / 3.0;
  vec2 uv = v_uv;
  vec3 acc = vec3(0.0);
  float wsum = 0.0;
  float radius = mix(0.0, 0.45, rough * rough);
  for (int y = -2; y <= 2; ++y) {
    for (int x = -2; x <= 2; ++x) {
      vec2 o = vec2(float(x), float(y)) / 2.0;
      float d = length(o);
      float w = exp(-d * d / max(rough * 2.0 + 0.05, 0.05));
      if (radius <= 0.0) w = (x == 0 && y == 0) ? 1.0 : 0.0;
      vec2 s = clamp(uv + o * radius, 0.0, 1.0);
      vec2 atlas = vec2((s.x + face) / 6.0, s.y);
      acc += texture(u_albedo_tex, atlas).rgb * w;
      wsum += w;
    }
  }
  o_color = vec4(acc / max(wsum, 0.0001), 1.0);
}
