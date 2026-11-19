// Shared frame/draw blocks for the forward renderer.
// Authored as Vulkan GLSL 450. The build cross-compiles this to SPIR-V,
// GLSL 330 (OpenGL) and HLSL / DXIL (D3D12). std140 matches FrameBlock /
// DrawBlock in src/render3d/RhiRenderer3D.cpp.

layout(std140, set = 0, binding = 0) uniform Frame {
  mat4 view_proj;
  mat4 inv_view_proj;
  mat4 light_vp[4];
  vec4 cam;
  vec4 sun_dir;
  vec4 sun_color;
  vec4 sky_top;
  vec4 sky_horizon;
  vec4 ground;
  vec4 fog;
  vec4 fog_extra;       // height, falloff, shadow_texel, ambient
  vec4 counts;          // points, spots, areas, cascades
  vec4 flags;           // shadows, filter (0 pcf / 1 pcss), tonemap (1 = in shader), exposure
  vec4 wind;            // dir.x, dir.z, strength, gust
  vec4 timev;           // seconds, pcss light size, ssao radius, ssao strength
  vec4 cascade_splits;  // view-space distances
  vec4 atlas;           // tiles_x, tiles_y, map size, flip shadow v
  vec4 clip;            // y scale, z scale, z bias, unused
  vec4 grade;           // contrast, saturation, gain, bloom strength
  vec4 point_pos[8];
  vec4 point_color[8];  // rgb, range
  vec4 spot_pos[4];
  vec4 spot_dir[4];     // xyz, cos outer
  vec4 spot_color[4];   // rgb, cos inner
  vec4 area_pos[2];
  vec4 area_u[2];
  vec4 area_v[2];
  vec4 area_color[2];
  vec4 probe_pos[4];    // xyz, radius
  vec4 probe_color[4];
  // Appended. Classic leaves these at zero and never samples them.
  mat4 local_vp[8];     // spot map, or one cube face of a point light
  vec4 local_rect[8];   // atlas uv origin xy, uv size, depth bias
  vec4 local_soft[8];   // softness, 2 = cube face, 0 = empty
  vec4 refl_pos[2];     // xyz centre, sphere radius (0 = none)
  vec4 refl_ext[2];     // xyz box half-extent (0 = sphere), blend distance
  vec4 gi_origin;       // volume min corner, probe count
  vec4 gi_spacing;      // xyz spacing, 1 when SH is valid
  vec4 gi_dim;          // nx, ny, nz
  vec4 gi_sh[96];       // 32 probes, 3 vec4 each (L1 red, green, blue)
  vec4 ibl;             // x = RGBM range (0 = straight colour)
} frame;

layout(std140, set = 0, binding = 1) uniform Draw {
  mat4 model;
  vec4 albedo;
  vec4 emissive;        // xyz + unlit
  vec4 mr;              // metallic, roughness, alpha cutoff, height scale
  vec4 draw_flags;      // has_normal, alpha mode, skinned, terrain
  vec4 extra;           // double sided, pom, foliage part colors, sway
  vec4 layer_tint[4];
  vec4 layer_tiling;
  vec4 foliage_a;       // color a rgb, mesh height
  vec4 foliage_b;       // color b rgb, unused
} draw;

#ifdef TS_SKIN
layout(std140, set = 0, binding = 2) uniform Bones {
  mat4 u_bones[128];
} bones;
#endif

vec4 ts_clip(vec4 c) {
  c.y *= frame.clip.x;
  c.z = c.z * frame.clip.y + c.w * frame.clip.z;
  return c;
}

float ts_fog_amount(vec3 world) {
  if (frame.fog.a <= 0.0) return 0.0;
  float d = length(world - frame.cam.xyz);
  float h = 0.5 * (world.y + frame.cam.y) - frame.fog_extra.x;
  float k = frame.fog.a * exp(-max(h, 0.0) * frame.fog_extra.y);
  return clamp(1.0 - exp(-d * k), 0.0, 1.0);
}

vec3 ts_apply_fog(vec3 display_color, vec3 world) {
  float amt = ts_fog_amount(world);
  // sun_dir.w marks volumetric fog: a soft shaft toward the sun.
  if (frame.sun_dir.w > 0.5 && frame.fog.a > 0.0) {
    vec3 view = normalize(world - frame.cam.xyz);
    float shafts = pow(clamp(dot(view, normalize(-frame.sun_dir.xyz)), 0.0, 1.0), 8.0);
    amt = clamp(amt + shafts * frame.fog.a * 4.0, 0.0, 1.0);
  }
  return mix(display_color, frame.fog.rgb, amt);
}

vec3 ts_sky_fog(vec3 c, vec3 dir) {
  if (frame.fog.a <= 0.0) return c;
  float k = clamp(frame.fog.a * 60.0, 0.0, 1.0);
  float low = 1.0 - smoothstep(-0.05, 0.45, dir.y);
  return mix(c, frame.fog.rgb, k * mix(0.35, 1.0, low));
}

// Skinning lives in the vertex stage (it reads vertex attributes).
