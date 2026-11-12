#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  float d = texture(u_shadow, v_uv).r;
  float occ = 0.0;
  float r = frame.timev.z * 0.01;
  for (int i = 0; i < 8; ++i) {
    float a = float(i) * 2.399;
    vec2 o = vec2(cos(a), sin(a)) * r * (0.4 + 0.6 * float(i) / 8.0);
    float s = texture(u_shadow, v_uv + o).r;
    occ += s < d - 0.001 ? 1.0 : 0.0;
  }
  float ao = 1.0 - occ / 8.0 * frame.timev.w;
  o_color = vec4(ao, ao, ao, 1.0);
}
