#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef TS_RAY
#extension GL_EXT_ray_query : require
#endif
#include "common.glsl"
#include "samplers.glsl"

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_nrm;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tan;
layout(location = 4) in vec4 v_light0;
layout(location = 5) in vec4 v_light1;
layout(location = 6) in vec4 v_light2;
layout(location = 7) in vec4 v_light3;
layout(location = 8) in float v_view_z;
layout(location = 9) in float v_part;
layout(location = 10) in float v_fade;
layout(location = 0) out vec4 o_color;

const float PI = 3.14159265;

#ifdef TS_RAY
layout(set = 0, binding = 15) uniform accelerationStructureEXT u_tlas;
#endif

float ts_bayer(vec2 p) {
  int x = int(mod(p.x, 4.0));
  int y = int(mod(p.y, 4.0));
  int i = x + y * 4;
  int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  return (float(m[i]) + 0.5) / 16.0;
}

vec4 ts_surface(vec2 uv, vec3 world) {
  if (draw.draw_flags.w < 0.5) return draw.albedo * texture(u_albedo_tex, uv);
  vec4 w = texture(u_splat, uv);
  w /= max(w.r + w.g + w.b + w.a, 0.0001);
  vec3 c = texture(u_layer0, world.xz / draw.layer_tiling.x).rgb * draw.layer_tint[0].rgb * w.r
         + texture(u_layer1, world.xz / draw.layer_tiling.y).rgb * draw.layer_tint[1].rgb * w.g
         + texture(u_layer2, world.xz / draw.layer_tiling.z).rgb * draw.layer_tint[2].rgb * w.b
         + texture(u_layer3, world.xz / draw.layer_tiling.w).rgb * draw.layer_tint[3].rgb * w.a;
  float macro = texture(u_layer0, world.xz / (draw.layer_tiling.x * 9.7)).g;
  c *= 0.85 + 0.3 * macro;
  return vec4(c * draw.albedo.rgb, 1.0);
}

vec2 ts_atlas(vec2 uv, int cascade) {
  float tx = max(frame.atlas.x, 1.0);
  float ty = max(frame.atlas.y, 1.0);
  float col = float(cascade - (cascade / int(tx)) * int(tx));
  float row = float(cascade / int(tx));
  return (uv + vec2(col, row)) / vec2(tx, ty);
}

float ts_pcf(vec2 uv, float ref, float texel, int radius) {
  float s = 0.0;
  float n = 0.0;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      float d = texture(u_shadow, uv + vec2(float(x), float(y)) * texel).r;
      s += (ref <= d) ? 1.0 : 0.0;
      n += 1.0;
    }
  }
  return s / n;
}

float ts_shadow_map(vec4 light, vec3 n, int cascade) {
  vec3 ndc = light.xyz / light.w;
  vec2 uv = ndc.xy * 0.5 + 0.5;
  if (frame.atlas.w > 0.5) uv.y = 1.0 - uv.y;
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
  float ref = ndc.z * 0.5 + 0.5;
  float bias = max(0.0025 * (1.0 - dot(n, -frame.sun_dir.xyz)), 0.0006);
  ref -= bias;
  vec2 auv = ts_atlas(uv, cascade);
  float tiles = max(frame.atlas.x, 1.0);
  float texel = frame.fog_extra.z * tiles;
  if (frame.flags.y < 0.5) return ts_pcf(auv, ref, texel, 1);
  // PCSS: blocker search, then a kernel that grows with the penumbra.
  float search = 0.0;
  float blockers = 0.0;
  float wide = texel * 6.0;
  for (int y = -2; y <= 2; ++y) {
    for (int x = -2; x <= 2; ++x) {
      float d = texture(u_shadow, auv + vec2(float(x), float(y)) * wide).r;
      if (d < ref) {
        search += d;
        blockers += 1.0;
      }
    }
  }
  if (blockers < 1.0) return 1.0;
  float avg = search / blockers;
  float pen = clamp((ref - avg) / max(avg, 0.001) * frame.timev.y, 1.0, 8.0);
  return ts_pcf(auv, ref, texel * pen, 2);
}

int ts_cascade() {
  int n = int(frame.counts.w + 0.5);
  if (n <= 1) return 0;
  if (v_view_z < frame.cascade_splits.x) return 0;
  if (n > 1 && v_view_z < frame.cascade_splits.y) return 1;
  if (n > 2 && v_view_z < frame.cascade_splits.z) return 2;
  return n - 1;
}

float ts_sun_shadow(vec3 n) {
  if (frame.flags.x < 0.5) return 1.0;
#ifdef TS_RAY
  vec3 origin = v_world + n * 0.05;
  vec3 dir = normalize(-frame.sun_dir.xyz);
  rayQueryEXT q;
  rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, origin, 0.02, dir, 200.0);
  while (rayQueryProceedEXT(q)) {}
  if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) return 0.15;
  return 1.0;
#else
  int c = ts_cascade();
  vec4 lv = c == 0 ? v_light0 : (c == 1 ? v_light1 : (c == 2 ? v_light2 : v_light3));
  return ts_shadow_map(lv, n, c);
#endif
}

vec3 ts_brdf(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, float metal, float rough) {
  vec3 h = normalize(v + l);
  float ndl = max(dot(n, l), 0.0);
  float ndv = max(dot(n, v), 0.001);
  float ndh = max(dot(n, h), 0.0);
  float a = rough * rough;
  float a2 = a * a;
  float d = a2 / (PI * pow(ndh * ndh * (a2 - 1.0) + 1.0, 2.0));
  float k = (rough + 1.0) * (rough + 1.0) / 8.0;
  float g = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
  vec3 f0 = mix(vec3(0.04), albedo, metal);
  vec3 f = f0 + (1.0 - f0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);
  vec3 spec = d * g * f / (4.0 * ndv * max(ndl, 0.001) + 0.0001);
  vec3 kd = (1.0 - f) * (1.0 - metal);
  return (kd * albedo / PI + spec) * radiance * ndl;
}

vec2 ts_pom(vec2 uv, vec3 n, vec3 view_dir) {
  float scale = draw.mr.w;
  if (scale <= 0.0 || draw.extra.y < 0.5 || draw.draw_flags.x < 0.5) return uv;
  vec3 t = normalize(v_tan.xyz - n * dot(n, v_tan.xyz));
  vec3 b = cross(n, t) * v_tan.w;
  mat3 tbn = transpose(mat3(t, b, n));
  vec3 v = normalize(tbn * view_dir);
  const int steps = 8;
  float layer = 1.0 / float(steps);
  vec2 duv = v.xy * scale / max(v.z, 0.2) / float(steps);
  float h = texture(u_normal_tex, uv).a;
  float cur = 1.0;
  vec2 p = uv;
  for (int i = 0; i < steps && cur > h; ++i) {
    p -= duv;
    h = texture(u_normal_tex, p).a;
    cur -= layer;
  }
  return p;
}

vec3 ts_nearest_probe(vec3 world) {
  vec3 c = vec3(0.0);
  float wsum = 0.0;
  for (int i = 0; i < 4; ++i) {
    float rad = frame.probe_pos[i].w;
    if (rad <= 0.0) continue;
    float d = length(world - frame.probe_pos[i].xyz);
    float w = clamp(1.0 - d / rad, 0.0, 1.0);
    c += frame.probe_color[i].rgb * w;
    wsum += w;
  }
  if (wsum <= 0.0) return vec3(0.0);
  return c / wsum;
}

void main() {
#ifdef TS_FOLIAGE
  if (v_fade < 0.999 && ts_bayer(gl_FragCoord.xy) > v_fade) discard;
#endif
  vec3 n0 = normalize(v_nrm);
  if (!gl_FrontFacing) n0 = -n0;
  vec3 view_dir = normalize(frame.cam.xyz - v_world);
  vec2 uv = ts_pom(v_uv, n0, view_dir);
#ifdef TS_FOLIAGE
  vec3 base_rgb = draw.extra.z > 0.5 ? mix(draw.foliage_a.rgb, draw.foliage_b.rgb, v_part) : draw.foliage_b.rgb;
  vec4 base = vec4(base_rgb, 1.0);
#else
  vec4 base = ts_surface(uv, v_world);
  if (int(draw.draw_flags.y + 0.5) == 1 && base.a < draw.mr.z) discard;
  if (draw.emissive.w > 0.5) {
    o_color = vec4(base.rgb, base.a);
    return;
  }
#endif
  vec3 albedo = pow(base.rgb, vec3(2.2));
  vec4 mr = texture(u_mr_tex, uv);
  float rough = clamp(draw.mr.y * mr.g, 0.04, 1.0);
  float metal = clamp(draw.mr.x * mr.b, 0.0, 1.0);
  vec3 n = n0;
  if (draw.draw_flags.x > 0.5) {
    vec3 t = normalize(v_tan.xyz - n * dot(n, v_tan.xyz));
    vec3 b = cross(n, t) * v_tan.w;
    vec3 tn = texture(u_normal_tex, uv).xyz * 2.0 - 1.0;
    n = normalize(mat3(t, b, n) * tn);
  }
  vec3 color = vec3(0.0);
  vec3 l = normalize(-frame.sun_dir.xyz);
#ifdef TS_FOLIAGE
  float ndl = max((dot(n, l) + 0.35) / 1.35, 0.0);
  color = albedo / PI * frame.sun_color.rgb * ndl * ts_sun_shadow(n);
#else
  color = ts_brdf(n, view_dir, l, frame.sun_color.rgb, albedo, metal, rough) * ts_sun_shadow(n);
  int np = int(frame.counts.x + 0.5);
  for (int i = 0; i < 8; ++i) {
    if (i >= np) break;
    vec3 d = frame.point_pos[i].xyz - v_world;
    float dist = length(d);
    float range = max(frame.point_color[i].w, 0.001);
    float fall = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
    fall = fall * fall / (dist * dist + 1.0);
    color += ts_brdf(n, view_dir, d / max(dist, 0.0001), frame.point_color[i].rgb * fall, albedo, metal, rough);
  }
  int ns = int(frame.counts.y + 0.5);
  for (int i = 0; i < 4; ++i) {
    if (i >= ns) break;
    vec3 d = frame.spot_pos[i].xyz - v_world;
    float dist = length(d);
    vec3 ld = d / max(dist, 0.0001);
    float cd = dot(normalize(-frame.spot_dir[i].xyz), ld);
    float outer = frame.spot_dir[i].w;
    float inner = frame.spot_color[i].w;
    float cone = clamp((cd - outer) / max(inner - outer, 0.001), 0.0, 1.0);
    float range = 12.0;
    float fall = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
    fall = fall * fall / (dist * dist + 1.0) * cone;
    color += ts_brdf(n, view_dir, ld, frame.spot_color[i].rgb * fall, albedo, metal, rough);
  }
  int na = int(frame.counts.z + 0.5);
  for (int i = 0; i < 2; ++i) {
    if (i >= na) break;
    vec3 cu = frame.area_u[i].xyz;
    vec3 cv = frame.area_v[i].xyz;
    vec3 center = frame.area_pos[i].xyz;
    vec3 to = center - v_world;
    float dist = length(to);
    vec3 ld = to / max(dist, 0.0001);
    float wrap = clamp(dot(n, normalize(cu)) * 0.15 + dot(n, normalize(cv)) * 0.15, 0.0, 0.5);
    float ndl_a = clamp(dot(n, ld) + wrap, 0.0, 1.0);
    float fall = 1.0 / (dist * dist + 1.0);
    color += albedo / PI * frame.area_color[i].rgb * ndl_a * fall;
  }
#endif
  vec3 sky = mix(pow(frame.ground.rgb, vec3(2.2)),
                 mix(pow(frame.sky_horizon.rgb, vec3(2.2)), pow(frame.sky_top.rgb, vec3(2.2)), clamp(n.y, 0.0, 1.0)),
                 clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
  vec3 probe = ts_nearest_probe(v_world);
  if (dot(probe, probe) > 0.0) sky = mix(sky, probe, 0.65);
  vec3 f0 = mix(vec3(0.04), albedo, metal);
  color += frame.fog_extra.w * sky * (albedo * (1.0 - metal) + f0 * (1.0 - rough) * 0.5);
#ifndef TS_FOLIAGE
  color += draw.emissive.rgb;
#endif
  vec3 mapped = color;
  if (frame.flags.z < 0.5) {
    mapped = color / (color + vec3(1.0));
    mapped *= frame.flags.w;
  }
  o_color = vec4(ts_apply_fog(pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2)), v_world), base.a);
}
