#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "samplers.glsl"
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;
void main() {
  vec4 a = frame.inv_view_proj * vec4(v_ndc, -1.0, 1.0);
  vec4 b = frame.inv_view_proj * vec4(v_ndc, 1.0, 1.0);
  vec3 d = normalize(b.xyz / b.w - a.xyz / a.w);
  vec3 c;
  if (frame.clip.w > 0.5) {
    vec2 uv = vec2(atan(d.z, d.x) / 6.2831853 + 0.5, asin(clamp(d.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    c = texture(u_hdri, uv).rgb;
  } else if (d.y >= 0.0) {
    c = mix(frame.sky_horizon.rgb, frame.sky_top.rgb, pow(d.y, 0.55));
  } else {
    c = mix(frame.sky_horizon.rgb, frame.ground.rgb, clamp(-d.y * 6.0, 0.0, 1.0));
  }
  float s = max(dot(d, -frame.sun_dir.xyz), 0.0);
  c += frame.sun_color.rgb * (pow(s, 900.0) * 1.4 + pow(s, 12.0) * 0.18);
  c = ts_sky_fog(c, d);
  o_color = vec4(c, 1.0);
}
