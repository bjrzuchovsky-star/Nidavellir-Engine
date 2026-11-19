#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 ts_world(vec2 uv, float depth) {
  vec2 ndc = uv * 2.0 - 1.0;
  if (frame.atlas.w > 0.5) ndc.y = -ndc.y;
  float z = depth * 2.0 - 1.0;
  vec4 w = frame.inv_view_proj * vec4(ndc.x, ndc.y, z, 1.0);
  return w.xyz / max(w.w, 0.0001);
}

vec3 ts_project(vec3 world, out float window_z) {
  vec4 c = ts_clip(frame.view_proj * vec4(world, 1.0));
  vec3 ndc = c.xyz / c.w;
  window_z = frame.clip.y > 0.75 ? ndc.z * 0.5 + 0.5 : ndc.z;
  vec2 uv = ndc.xy * 0.5 + 0.5;
  if (frame.atlas.w > 0.5) uv.y = 1.0 - uv.y;
  return vec3(uv, window_z);
}

void main() {
  vec4 scene = texture(u_albedo_tex, v_uv);
  float depth = texture(u_shadow, v_uv).r;
  if (depth <= 0.0 || depth >= 0.999) {
    o_color = scene;
    return;
  }
  float rough = clamp(scene.a, 0.0, 1.0);
  vec3 world = ts_world(v_uv, depth);
  vec3 view = normalize(frame.cam.xyz - world);
  vec3 n = normalize(ts_world(v_uv, depth) - ts_world(v_uv + vec2(1.0 / 160.0, 0.0), depth));
  n = normalize(cross(n, ts_world(v_uv + vec2(0.0, 1.0 / 90.0), depth) - world));
  if (dot(n, view) < 0.0) n = -n;
  vec3 rdir = reflect(-view, n);
  vec3 hit_color = vec3(0.0);
  float hit = 0.0;
  float traveled = 0.15;
  float step = 0.35;
  for (int i = 0; i < 16; ++i) {
    vec3 p = world + rdir * traveled;
    float wz;
    vec3 proj = ts_project(p, wz);
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) break;
    float sd = texture(u_shadow, proj.xy).r;
    if (sd < 0.999 && wz > sd + 0.002 && wz < sd + 0.08) {
      float fine = traveled;
      for (int k = 0; k < 4; ++k) {
        fine -= step * 0.5;
        vec3 q = world + rdir * fine;
        float qz;
        vec3 qp = ts_project(q, qz);
        float qd = texture(u_shadow, clamp(qp.xy, 0.0, 1.0)).r;
        if (qz > qd + 0.001) fine -= step * 0.25;
        else fine += step * 0.25;
      }
      vec3 at = world + rdir * fine;
      float az;
      vec3 ap = ts_project(at, az);
      hit_color = texture(u_albedo_tex, clamp(ap.xy, 0.0, 1.0)).rgb;
      float edge = min(min(ap.x, 1.0 - ap.x), min(ap.y, 1.0 - ap.y));
      hit = smoothstep(0.0, 0.08, edge);
      break;
    }
    traveled += step;
    step *= 1.15;
  }
  vec3 sky = mix(frame.sky_horizon.rgb, frame.sky_top.rgb, clamp(rdir.y * 0.5 + 0.5, 0.0, 1.0));
  vec3 env = hit > 0.0 ? hit_color : sky;
  float fres = pow(1.0 - max(dot(n, view), 0.0), 4.0);
  float fade = (1.0 - rough) * mix(0.15, 1.0, fres);
  o_color = vec4(mix(scene.rgb, env, fade * max(hit, 0.35)), scene.a);
}
