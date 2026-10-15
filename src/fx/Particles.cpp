#include "fx/Particles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/MemImages.h"

namespace ts {
namespace fx {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kPxPerMetre = 40.0f;

float q8(float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f; }

void set4(float* d, float r, float g, float b, float a) {
  d[0] = r;
  d[1] = g;
  d[2] = b;
  d[3] = a;
}
void set3(float* d, float x, float y, float z) {
  d[0] = x;
  d[1] = y;
  d[2] = z;
}

const char* shape_name(EmitShape s) {
  switch (s) {
    case EmitShape::Circle: return "circle";
    case EmitShape::Box: return "box";
    case EmitShape::Line: return "line";
    default: return "point";
  }
}

EmitShape shape_from(const std::string& s) {
  if (s == "circle") return EmitShape::Circle;
  if (s == "box") return EmitShape::Box;
  if (s == "line") return EmitShape::Line;
  return EmitShape::Point;
}

std::string num(float v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
  // Shortest that reads back the same float.
  for (int p = 1; p <= 9; ++p) {
    char t[32];
    std::snprintf(t, sizeof t, "%.*g", p, static_cast<double>(v));
    if (std::strtof(t, nullptr) == v) return t;
  }
  return buf;
}

std::string hex(const float* c) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", static_cast<int>(std::lround(q8(c[0]) * 255)),
                static_cast<int>(std::lround(q8(c[1]) * 255)),
                static_cast<int>(std::lround(q8(c[2]) * 255)),
                static_cast<int>(std::lround(q8(c[3]) * 255)));
  return buf;
}

bool parse_hex(const std::string& s, float* c) {
  if (s.size() != 9 || s[0] != '#') return false;
  for (int i = 0; i < 4; ++i) {
    char* end = nullptr;
    const std::string part = s.substr(1 + i * 2, 2);
    const long v = std::strtol(part.c_str(), &end, 16);
    if (!end || *end) return false;
    c[i] = static_cast<float>(v) / 255.0f;
  }
  return true;
}

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\';
    if (static_cast<unsigned char>(ch) < 0x20) continue;
    o += ch;
  }
  return o + "\"";
}

void lerp4(const float* a, const float* b, float t, float* o) {
  for (int i = 0; i < 4; ++i) o[i] = a[i] + (b[i] - a[i]) * t;
}

// Presets in metres, y up (3D). 2D scales by kPxPerMetre and flips y.
EmitterDesc base(const char* name) {
  EmitterDesc d;
  d.preset = name;
  set3(d.direction, 0, 1, 0);
  return d;
}

std::optional<EmitterDesc> preset_metres(const std::string& n) {
  EmitterDesc d = base(n.c_str());
  if (n == "dust") {
    d.continuous = false;
    d.burst = 10;
    d.shape = EmitShape::Circle;
    d.radius = 0.15f;
    d.spread = 70;
    d.speed_min = 0.3f;
    d.speed_max = 0.9f;
    set3(d.gravity, 0, -0.4f, 0);
    d.drag = 2.0f;
    d.life_min = 0.4f;
    d.life_max = 0.8f;
    set3(d.size, 0.12f, 0.25f, 0.35f);
    set4(d.color_start, 0.78f, 0.66f, 0.5f, 0.7f);
    set4(d.color_mid, 0.75f, 0.64f, 0.5f, 0.45f);
    set4(d.color_end, 0.72f, 0.62f, 0.5f, 0.0f);
  } else if (n == "gun_smoke") {
    d.continuous = false;
    d.burst = 14;
    set3(d.direction, 1, 0.2f, 0);
    d.spread = 25;
    d.speed_min = 0.5f;
    d.speed_max = 1.5f;
    set3(d.gravity, 0, 0.3f, 0);
    d.drag = 2.5f;
    d.life_min = 0.8f;
    d.life_max = 1.6f;
    set3(d.size, 0.1f, 0.35f, 0.6f);
    set4(d.color_start, 0.75f, 0.75f, 0.75f, 0.6f);
    set4(d.color_mid, 0.6f, 0.6f, 0.6f, 0.35f);
    set4(d.color_end, 0.5f, 0.5f, 0.5f, 0.0f);
    d.spin = 60;
  } else if (n == "muzzle_flash") {
    d.continuous = false;
    d.burst = 8;
    set3(d.direction, 1, 0, 0);
    d.spread = 15;
    d.speed_min = 2;
    d.speed_max = 4;
    d.drag = 8;
    d.life_min = 0.05f;
    d.life_max = 0.12f;
    set3(d.size, 0.15f, 0.22f, 0.1f);
    set4(d.color_start, 1, 0.9f, 0.5f, 1);
    set4(d.color_mid, 1, 0.6f, 0.2f, 0.9f);
    set4(d.color_end, 1, 0.3f, 0.1f, 0);
    d.blend = Blend::Additive;
  } else if (n == "sparks") {
    d.continuous = false;
    d.burst = 24;
    d.spread = 80;
    d.speed_min = 2;
    d.speed_max = 5;
    set3(d.gravity, 0, -9.8f, 0);
    d.drag = 0.5f;
    d.life_min = 0.3f;
    d.life_max = 0.7f;
    set3(d.size, 0.06f, 0.05f, 0.02f);
    set4(d.color_start, 1, 0.95f, 0.6f, 1);
    set4(d.color_mid, 1, 0.6f, 0.2f, 1);
    set4(d.color_end, 0.9f, 0.3f, 0.1f, 0);
    d.blend = Blend::Additive;
  } else if (n == "campfire") {
    d.rate = 30;
    d.shape = EmitShape::Circle;
    d.radius = 0.2f;
    d.spread = 15;
    d.speed_min = 0.4f;
    d.speed_max = 0.9f;
    set3(d.gravity, 0, 0.6f, 0);
    d.drag = 1;
    d.life_min = 0.5f;
    d.life_max = 1.0f;
    set3(d.size, 0.3f, 0.22f, 0.05f);
    set4(d.color_start, 1, 0.8f, 0.3f, 0.9f);
    set4(d.color_mid, 1, 0.4f, 0.1f, 0.7f);
    set4(d.color_end, 0.4f, 0.1f, 0.05f, 0);
    d.blend = Blend::Additive;
  } else if (n == "rain") {
    d.rate = 300;
    d.max_particles = 1024;
    d.shape = EmitShape::Box;
    set3(d.extent, 8, 0, 8);
    set3(d.direction, 0.08f, -1, 0);
    d.spread = 3;
    d.speed_min = 9;
    d.speed_max = 11;
    d.life_min = 0.9f;
    d.life_max = 1.1f;
    set3(d.size, 0.05f, 0.05f, 0.05f);
    set4(d.color_start, 0.6f, 0.7f, 0.9f, 0.5f);
    set4(d.color_mid, 0.6f, 0.7f, 0.9f, 0.5f);
    set4(d.color_end, 0.6f, 0.7f, 0.9f, 0.3f);
  } else if (n == "sand_storm") {
    d.rate = 120;
    d.max_particles = 600;
    d.shape = EmitShape::Box;
    set3(d.extent, 6, 2, 6);
    set3(d.direction, 1, 0.05f, 0);
    d.spread = 10;
    d.speed_min = 3;
    d.speed_max = 6;
    d.life_min = 1.5f;
    d.life_max = 3;
    set3(d.size, 0.2f, 0.5f, 0.8f);
    set4(d.color_start, 0.85f, 0.68f, 0.45f, 0);
    set4(d.color_mid, 0.82f, 0.65f, 0.42f, 0.35f);
    set4(d.color_end, 0.8f, 0.6f, 0.4f, 0);
    d.spin = 30;
  } else if (n == "thruster") {
    d.rate = 80;
    set3(d.direction, 0, -1, 0);
    d.spread = 12;
    d.speed_min = 2;
    d.speed_max = 3.5f;
    d.drag = 2;
    d.life_min = 0.2f;
    d.life_max = 0.45f;
    set3(d.size, 0.18f, 0.12f, 0.04f);
    set4(d.color_start, 0.6f, 0.85f, 1, 1);
    set4(d.color_mid, 1, 0.6f, 0.2f, 0.8f);
    set4(d.color_end, 0.4f, 0.3f, 0.3f, 0);
    d.blend = Blend::Additive;
  } else if (n == "explosion") {
    d.continuous = false;
    d.burst = 60;
    d.shape = EmitShape::Circle;
    d.radius = 0.2f;
    d.spread = 180;
    d.speed_min = 1;
    d.speed_max = 5;
    set3(d.gravity, 0, -1, 0);
    d.drag = 3;
    d.life_min = 0.4f;
    d.life_max = 1.1f;
    set3(d.size, 0.3f, 0.6f, 0.9f);
    set4(d.color_start, 1, 0.9f, 0.5f, 1);
    set4(d.color_mid, 1, 0.45f, 0.15f, 0.8f);
    set4(d.color_end, 0.25f, 0.2f, 0.2f, 0);
    d.blend = Blend::Additive;
  } else if (n == "sparkle") {
    d.continuous = false;
    d.burst = 12;
    d.shape = EmitShape::Circle;
    d.radius = 0.08f;
    d.spread = 180;
    d.speed_min = 0.1f;
    d.speed_max = 0.4f;
    d.drag = 1;
    d.life_min = 0.3f;
    d.life_max = 0.6f;
    set3(d.size, 0.03f, 0.05f, 0.0f);
    set4(d.color_start, 1, 1, 0.8f, 1);
    set4(d.color_mid, 1, 0.9f, 0.5f, 1);
    set4(d.color_end, 1, 0.8f, 0.4f, 0);
    d.blend = Blend::Additive;
  } else {
    return std::nullopt;
  }
  return d;
}

}  // namespace

void normalize(EmitterDesc& d) {
  d.max_particles = std::clamp(d.max_particles, 1, EmitterDesc::kMaxParticles);
  d.burst = std::clamp(d.burst, 0, EmitterDesc::kMaxParticles);
  d.rate = std::clamp(std::isfinite(d.rate) ? d.rate : 0.0f, 0.0f, 5000.0f);
  d.duration = std::max(0.0f, std::isfinite(d.duration) ? d.duration : 0.0f);
  d.radius = std::max(0.0f, d.radius);
  for (float& e : d.extent) e = std::max(0.0f, e);
  d.spread = std::clamp(d.spread, 0.0f, 180.0f);
  d.speed_min = std::max(0.0f, d.speed_min);
  d.speed_max = std::max(d.speed_min, d.speed_max);
  d.drag = std::clamp(d.drag, 0.0f, 20.0f);
  d.life_min = std::clamp(d.life_min, 0.01f, 60.0f);
  d.life_max = std::clamp(d.life_max, d.life_min, 60.0f);
  d.mid_t = std::clamp(d.mid_t, 0.01f, 0.99f);
  for (float& s : d.size) s = std::max(0.0f, s);
  for (float* c : {d.color_start, d.color_mid, d.color_end}) {
    for (int i = 0; i < 4; ++i) c[i] = q8(c[i]);
  }
  d.spin = std::clamp(d.spin, 0.0f, 3600.0f);
  d.frames_x = std::clamp(d.frames_x, 1, 64);
  d.frames_y = std::clamp(d.frames_y, 1, 64);
  d.frame_fps = std::clamp(d.frame_fps, 0.0f, 120.0f);
  if (d.direction[0] == 0 && d.direction[1] == 0 && d.direction[2] == 0) d.direction[1] = -1;
}

const std::vector<std::string>& preset_names() {
  static const std::vector<std::string> names = {
      "dust", "gun_smoke", "muzzle_flash", "sparks", "campfire",
      "rain", "sand_storm", "thruster", "explosion", "sparkle"};
  return names;
}

std::optional<EmitterDesc> preset(const std::string& name, Space space) {
  std::optional<EmitterDesc> d = preset_metres(name);
  if (!d) return d;
  if (space == Space::TwoD) {
    const float k = kPxPerMetre;
    d->radius *= k;
    d->extent[0] *= k;
    d->extent[1] *= k;
    d->extent[2] = 0;
    d->direction[1] = -d->direction[1];
    d->direction[2] = 0;
    d->speed_min *= k;
    d->speed_max *= k;
    d->gravity[0] *= k;
    d->gravity[1] *= -k;
    d->gravity[2] = 0;
    for (float& s : d->size) s *= k;
  }
  normalize(*d);
  return d;
}

std::vector<Field> to_fields(const EmitterDesc& d) {
  static const EmitterDesc def;
  std::vector<Field> f;
  auto put = [&](const char* k, const std::string& v) { f.push_back({k, v}); };
  auto flt = [&](const char* k, float v, float dv) {
    if (v != dv) put(k, num(v));
  };
  auto flag = [&](const char* k, bool v, bool dv) {
    if (v != dv) put(k, v ? "true" : "false");
  };
  if (!d.preset.empty()) put("preset", quote(d.preset));
  flag("enabled", d.enabled, def.enabled);
  flag("continuous", d.continuous, def.continuous);
  flt("rate", d.rate, def.rate);
  if (d.burst != def.burst) put("burst", std::to_string(d.burst));
  flt("duration", d.duration, def.duration);
  if (d.max_particles != def.max_particles) put("max_particles", std::to_string(d.max_particles));
  if (d.shape != def.shape) put("shape", quote(shape_name(d.shape)));
  flt("radius", d.radius, def.radius);
  static const char* ax[3] = {"x", "y", "z"};
  for (int i = 0; i < 3; ++i) {
    flt(("extent_" + std::string(ax[i])).c_str(), d.extent[i], def.extent[i]);
  }
  for (int i = 0; i < 3; ++i) {
    flt(("dir_" + std::string(ax[i])).c_str(), d.direction[i], def.direction[i]);
  }
  flt("spread", d.spread, def.spread);
  flt("speed_min", d.speed_min, def.speed_min);
  flt("speed_max", d.speed_max, def.speed_max);
  for (int i = 0; i < 3; ++i) {
    flt(("gravity_" + std::string(ax[i])).c_str(), d.gravity[i], def.gravity[i]);
  }
  flt("drag", d.drag, def.drag);
  flt("life_min", d.life_min, def.life_min);
  flt("life_max", d.life_max, def.life_max);
  flt("mid_t", d.mid_t, def.mid_t);
  flt("size_start", d.size[0], def.size[0]);
  flt("size_mid", d.size[1], def.size[1]);
  flt("size_end", d.size[2], def.size[2]);
  auto col = [&](const char* k, const float* c, const float* dc) {
    if (hex(c) != hex(dc)) put(k, quote(hex(c)));
  };
  col("color_start", d.color_start, def.color_start);
  col("color_mid", d.color_mid, def.color_mid);
  col("color_end", d.color_end, def.color_end);
  flt("spin", d.spin, def.spin);
  if (!d.texture.empty()) put("texture", quote(d.texture));
  if (d.frames_x != 1) put("frames_x", std::to_string(d.frames_x));
  if (d.frames_y != 1) put("frames_y", std::to_string(d.frames_y));
  flt("frame_fps", d.frame_fps, def.frame_fps);
  if (d.blend == Blend::Additive) put("blend", "\"additive\"");
  flag("local_space", d.local_space, def.local_space);
  if (d.seed) put("seed", std::to_string(d.seed));
  return f;
}

EmitterDesc from_fields(const FieldLookup& get) {
  EmitterDesc d;
  auto flt = [&](const char* k, float* v) {
    if (auto s = get(k)) {
      char* end = nullptr;
      const float x = std::strtof(s->c_str(), &end);
      if (end != s->c_str() && std::isfinite(x)) *v = x;
    }
  };
  auto integer = [&](const char* k, int* v) {
    float x = static_cast<float>(*v);
    flt(k, &x);
    *v = static_cast<int>(x);
  };
  auto flag = [&](const char* k, bool* v) {
    if (auto s = get(k)) *v = *s == "true";
  };
  auto str = [&](const char* k, std::string* v) {
    if (auto s = get(k)) *v = *s;
  };
  str("preset", &d.preset);
  flag("enabled", &d.enabled);
  flag("continuous", &d.continuous);
  flt("rate", &d.rate);
  integer("burst", &d.burst);
  flt("duration", &d.duration);
  integer("max_particles", &d.max_particles);
  if (auto s = get("shape")) d.shape = shape_from(*s);
  flt("radius", &d.radius);
  static const char* ax[3] = {"x", "y", "z"};
  for (int i = 0; i < 3; ++i) flt(("extent_" + std::string(ax[i])).c_str(), &d.extent[i]);
  for (int i = 0; i < 3; ++i) flt(("dir_" + std::string(ax[i])).c_str(), &d.direction[i]);
  flt("spread", &d.spread);
  flt("speed_min", &d.speed_min);
  flt("speed_max", &d.speed_max);
  for (int i = 0; i < 3; ++i) flt(("gravity_" + std::string(ax[i])).c_str(), &d.gravity[i]);
  flt("drag", &d.drag);
  flt("life_min", &d.life_min);
  flt("life_max", &d.life_max);
  flt("mid_t", &d.mid_t);
  flt("size_start", &d.size[0]);
  flt("size_mid", &d.size[1]);
  flt("size_end", &d.size[2]);
  if (auto s = get("color_start")) parse_hex(*s, d.color_start);
  if (auto s = get("color_mid")) parse_hex(*s, d.color_mid);
  if (auto s = get("color_end")) parse_hex(*s, d.color_end);
  flt("spin", &d.spin);
  str("texture", &d.texture);
  integer("frames_x", &d.frames_x);
  integer("frames_y", &d.frames_y);
  flt("frame_fps", &d.frame_fps);
  if (auto s = get("blend")) d.blend = *s == "additive" ? Blend::Additive : Blend::Alpha;
  flag("local_space", &d.local_space);
  if (auto s = get("seed")) d.seed = static_cast<std::uint32_t>(std::strtoul(s->c_str(), nullptr, 10));
  normalize(d);
  return d;
}

void ensure_builtin_images() {
  namespace mi = tombstone::mem_images;
  if (mi::get(kDotImage)) return;
  const int n = 64;
  std::vector<unsigned char> px(static_cast<std::size_t>(n) * n * 4);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const float dx = (x + 0.5f) / n * 2 - 1, dy = (y + 0.5f) / n * 2 - 1;
      const float r = std::sqrt(dx * dx + dy * dy);
      float a = std::clamp(1.0f - r, 0.0f, 1.0f);
      a = a * a * (3 - 2 * a);
      unsigned char* p = &px[(static_cast<std::size_t>(y) * n + x) * 4];
      p[0] = p[1] = p[2] = 255;
      p[3] = static_cast<unsigned char>(std::lround(a * 255));
    }
  }
  mi::put(kDotImage, n, n, std::move(px));
}

// ---- Emitter -------------------------------------------------------------------

Emitter::Emitter(const EmitterDesc& desc, std::uint32_t seed, Space space)
    : desc_(desc), rng_(seed), space_(space) {
  normalize(desc_);
}

void Emitter::set_desc(const EmitterDesc& desc) {
  const bool restart_needed = desc.burst != desc_.burst || desc.continuous != desc_.continuous;
  desc_ = desc;
  normalize(desc_);
  if (restart_needed) restart();
}

void Emitter::restart() {
  time_ = 0;
  accum_ = 0;
  started_ = false;
  stopped_ = false;
}

void Emitter::spawn(const float origin[3]) {
  if (static_cast<int>(parts_.size()) >= desc_.max_particles) return;
  Particle p{};
  float off[3] = {0, 0, 0};
  const bool flat = space_ == Space::TwoD;
  switch (desc_.shape) {
    case EmitShape::Circle: {
      const float a = rng_.range(0, 2 * kPi);
      const float r = desc_.radius * std::sqrt(rng_.unit());
      if (flat) {
        set3(off, std::cos(a) * r, std::sin(a) * r, 0);
      } else {
        set3(off, std::cos(a) * r, 0, std::sin(a) * r);
      }
      break;
    }
    case EmitShape::Box:
      for (int i = 0; i < 3; ++i) off[i] = rng_.range(-desc_.extent[i], desc_.extent[i]);
      break;
    case EmitShape::Line:
      off[0] = rng_.range(-desc_.extent[0], desc_.extent[0]);
      break;
    default:
      break;
  }
  float dir[3] = {desc_.direction[0], desc_.direction[1], desc_.direction[2]};
  const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  for (float& c : dir) c /= (len > 0 ? len : 1);
  const float spread = desc_.spread * kPi / 180.0f;
  if (flat) {
    const float a = std::atan2(dir[1], dir[0]) + rng_.range(-spread, spread);
    set3(dir, std::cos(a), std::sin(a), 0);
  } else {
    // Random direction within the cone around dir (uniform on the cap).
    const float cz = 1 - rng_.unit() * (1 - std::cos(spread));
    const float sz = std::sqrt(std::max(0.0f, 1 - cz * cz));
    const float phi = rng_.range(0, 2 * kPi);
    float t[3] = {0, 1, 0};
    if (std::fabs(dir[1]) > 0.9f) set3(t, 1, 0, 0);
    float u[3] = {dir[1] * t[2] - dir[2] * t[1], dir[2] * t[0] - dir[0] * t[2],
                  dir[0] * t[1] - dir[1] * t[0]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& c : u) c /= ul;
    const float w[3] = {dir[1] * u[2] - dir[2] * u[1], dir[2] * u[0] - dir[0] * u[2],
                        dir[0] * u[1] - dir[1] * u[0]};
    float nd[3];
    for (int i = 0; i < 3; ++i) {
      nd[i] = dir[i] * cz + (u[i] * std::cos(phi) + w[i] * std::sin(phi)) * sz;
    }
    set3(dir, nd[0], nd[1], nd[2]);
  }
  const float speed = rng_.range(desc_.speed_min, desc_.speed_max);
  for (int i = 0; i < 3; ++i) {
    p.p[i] = off[i] + (desc_.local_space ? 0.0f : origin[i]);
    p.v[i] = dir[i] * speed;
  }
  p.life = rng_.range(desc_.life_min, desc_.life_max);
  p.scale = rng_.range(0.8f, 1.2f);
  p.rot = rng_.range(0, 360);
  p.spin = rng_.range(-desc_.spin, desc_.spin);
  const int frames = desc_.frames_x * desc_.frames_y;
  p.frame0 = static_cast<std::uint16_t>(frames > 1 ? rng_.next() % frames : 0);
  parts_.push_back(p);
}

void Emitter::emit(int count, const float origin[3]) {
  count = std::clamp(count, 0, EmitterDesc::kMaxParticles);
  for (int i = 0; i < count; ++i) spawn(origin);
}

void Emitter::update(float dt, const float origin[3]) {
  set3(origin_, origin[0], origin[1], origin[2]);
  if (dt <= 0) return;
  dt = std::min(dt, 0.25f);
  // Age and move.
  const float damp = std::exp(-desc_.drag * dt);
  std::size_t w = 0;
  for (std::size_t i = 0; i < parts_.size(); ++i) {
    Particle p = parts_[i];
    p.age += dt;
    if (p.age >= p.life) continue;
    for (int k = 0; k < 3; ++k) {
      p.v[k] = (p.v[k] + (desc_.gravity[k] + wind_[k]) * dt) * damp;
      p.p[k] += p.v[k] * dt;
    }
    p.rot += p.spin * dt;
    parts_[w++] = p;
  }
  parts_.resize(w);
  if (stopped_ || !desc_.enabled) return;
  if (!started_) {
    started_ = true;
    emit(desc_.burst, origin);
  }
  const bool in_time = desc_.duration <= 0 || time_ < desc_.duration;
  if (desc_.continuous && in_time) {
    accum_ += desc_.rate * dt;
    const int n = static_cast<int>(accum_);
    accum_ -= static_cast<float>(n);
    emit(n, origin);
  }
  time_ += dt;
}

bool Emitter::finished() const {
  if (!parts_.empty()) return false;
  if (stopped_ || !desc_.enabled) return true;
  if (!started_) return false;
  return !desc_.continuous || (desc_.duration > 0 && time_ >= desc_.duration);
}

void Emitter::sprites(std::vector<Sprite>* out) const {
  static const std::string dot = kDotImage;
  const int frames = desc_.frames_x * desc_.frames_y;
  for (const Particle& p : parts_) {
    Sprite s;
    const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
    float sz;
    if (t < desc_.mid_t) {
      const float k = t / desc_.mid_t;
      sz = desc_.size[0] + (desc_.size[1] - desc_.size[0]) * k;
      lerp4(desc_.color_start, desc_.color_mid, k, s.rgba);
    } else {
      const float k = (t - desc_.mid_t) / (1 - desc_.mid_t);
      sz = desc_.size[1] + (desc_.size[2] - desc_.size[1]) * k;
      lerp4(desc_.color_mid, desc_.color_end, k, s.rgba);
    }
    if (s.rgba[3] <= 0.002f || sz <= 0.0f) continue;
    for (int k = 0; k < 3; ++k) s.pos[k] = p.p[k] + (desc_.local_space ? origin_[k] : 0.0f);
    s.size = sz * p.scale;
    s.rotation = p.rot;
    s.additive = desc_.blend == Blend::Additive;
    if (desc_.texture.empty()) {
      s.image = &dot;
    } else {
      s.image = &desc_.texture;
      if (frames > 1) {
        int f = desc_.frame_fps > 0
                    ? (p.frame0 + static_cast<int>(p.age * desc_.frame_fps)) % frames
                    : std::min(frames - 1, static_cast<int>(t * frames));
        const float cw = 1.0f / desc_.frames_x, ch = 1.0f / desc_.frames_y;
        s.u0 = (f % desc_.frames_x) * cw;
        s.v0 = (f / desc_.frames_x) * ch;
        s.u1 = s.u0 + cw;
        s.v1 = s.v0 + ch;
      }
    }
    out->push_back(s);
  }
}

double Emitter::checksum() const {
  double c = 0;
  for (const Particle& p : parts_) c += p.p[0] * 1.0 + p.p[1] * 3.0 + p.p[2] * 7.0;
  return c;
}

// ---- ParticleWorld --------------------------------------------------------------

void ParticleWorld::clear() {
  entities_.clear();
  order_.clear();
  shots_.clear();
  shot_seed_ = 0x51ED27u;
}

void ParticleWorld::sync(std::uint64_t entity, const EmitterDesc& desc, const float pos[3]) {
  auto it = entities_.find(entity);
  if (it == entities_.end()) {
    const std::uint32_t seed =
        desc.seed ? desc.seed
                  : static_cast<std::uint32_t>(entity * 2654435761u) ^ 0xA5A5A5A5u;
    Entry e;
    e.em = Emitter(desc, seed, space_);
    it = entities_.emplace(entity, std::move(e)).first;
    order_.push_back(entity);
  } else if (!(it->second.em.desc() == desc)) {
    EmitterDesc d = desc;
    normalize(d);
    if (!(it->second.em.desc() == d)) it->second.em.set_desc(d);
  }
  set3(it->second.pos, pos[0], pos[1], pos[2]);
  it->second.seen = true;
}

void ParticleWorld::drop(std::uint64_t entity) {
  entities_.erase(entity);
  order_.erase(std::remove(order_.begin(), order_.end(), entity), order_.end());
}

void ParticleWorld::prune() {
  std::vector<std::uint64_t> gone;
  for (std::uint64_t id : order_) {
    Entry& e = entities_[id];
    if (!e.seen) gone.push_back(id);
    e.seen = false;
  }
  for (std::uint64_t id : gone) drop(id);
}

void ParticleWorld::stop(std::uint64_t entity) {
  if (auto it = entities_.find(entity); it != entities_.end()) it->second.em.stop();
}

void ParticleWorld::start(std::uint64_t entity) {
  if (auto it = entities_.find(entity); it != entities_.end()) it->second.em.restart();
}

void ParticleWorld::burst(const EmitterDesc& desc, const float pos[3], int count) {
  if (shots_.size() >= 256) return;
  EmitterDesc d = desc;
  const int n = count > 0 ? count : (d.burst > 0 ? d.burst : std::max(1, static_cast<int>(d.rate * 0.5f)));
  d.continuous = false;
  d.burst = 0;
  d.enabled = true;
  shot_seed_ = shot_seed_ * 1664525u + 1013904223u;
  Entry e;
  e.em = Emitter(d, d.seed ? d.seed + shot_seed_ : shot_seed_, space_);
  set3(e.pos, pos[0], pos[1], pos[2]);
  e.em.emit(n, pos);
  e.em.stop();
  shots_.push_back(std::move(e));
}

bool ParticleWorld::emit(std::uint64_t entity, int count) {
  auto it = entities_.find(entity);
  if (it == entities_.end()) return false;
  it->second.em.emit(count, it->second.pos);
  return true;
}

void ParticleWorld::update(float dt) {
  for (std::uint64_t id : order_) {
    Entry& e = entities_[id];
    e.em.set_wind(wind_);
    e.em.update(dt, e.pos);
  }
  for (Entry& e : shots_) {
    e.em.set_wind(wind_);
    e.em.update(dt, e.pos);
  }
  shots_.erase(std::remove_if(shots_.begin(), shots_.end(),
                              [](const Entry& e) { return e.em.finished(); }),
               shots_.end());
}

void ParticleWorld::sprites(std::vector<Sprite>* out) const {
  for (std::uint64_t id : order_) entities_.at(id).em.sprites(out);
  for (const Entry& e : shots_) e.em.sprites(out);
}

int ParticleWorld::alive() const {
  int n = 0;
  for (const auto& [id, e] : entities_) n += e.em.alive();
  for (const Entry& e : shots_) n += e.em.alive();
  return n;
}

double ParticleWorld::checksum() const {
  double c = 0;
  for (std::uint64_t id : order_) c += entities_.at(id).em.checksum();
  for (const Entry& e : shots_) c += e.em.checksum();
  return c;
}

}  // namespace fx
}  // namespace ts
