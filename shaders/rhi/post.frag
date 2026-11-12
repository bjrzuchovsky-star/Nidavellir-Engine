#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) {
  const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
  vec2 uv = v_uv;
  vec3 color = texture(u_albedo_tex, uv).rgb;
  color = pow(max(color, vec3(0.0)), vec3(2.2));
  float ao = texture(u_ao, uv).r;
  color *= mix(1.0, ao, frame.timev.w);
  vec3 bloom = texture(u_bloom, uv).rgb;
  color += bloom * frame.grade.w;
  color *= frame.flags.w;
  if (frame.flags.z < 1.5) color = color / (color + vec3(1.0));
  else color = aces(color);
  color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
  float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
  color = mix(vec3(luma), color, frame.grade.y);
  color = (color - 0.5) * frame.grade.x + 0.5;
  color *= frame.grade.z;
  o_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
