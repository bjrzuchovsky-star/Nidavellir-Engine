#version 450
#extension GL_GOOGLE_include_directive : require
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  vec3 c = texture(u_albedo_tex, v_uv).rgb;
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  vec3 bright = c * smoothstep(0.75, 1.0, l);
  vec2 t = vec2(1.0 / 256.0);
  vec3 blur = bright * 0.2;
  blur += texture(u_albedo_tex, v_uv + vec2(t.x, 0.0)).rgb * 0.2;
  blur += texture(u_albedo_tex, v_uv - vec2(t.x, 0.0)).rgb * 0.2;
  blur += texture(u_albedo_tex, v_uv + vec2(0.0, t.y)).rgb * 0.2;
  blur += texture(u_albedo_tex, v_uv - vec2(0.0, t.y)).rgb * 0.2;
  o_color = vec4(mix(bright, blur, 0.65), 1.0);
}
