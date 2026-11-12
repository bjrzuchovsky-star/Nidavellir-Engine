#include "render3d/lighting/Lighting.h"

#include <cstdlib>

namespace ts {
namespace tombstone {
namespace render3d {

namespace {
std::vector<std::shared_ptr<LightingModule>>& modules() {
  static std::vector<std::shared_ptr<LightingModule>> m;
  return m;
}

class FnModule final : public LightingModule {
 public:
  const char* id_ = "";
  const char* label_ = "";
  bool (*get_)(const Lighting&) = nullptr;
  void (*set_)(Lighting*, bool) = nullptr;
  const char* id() const override { return id_; }
  const char* label() const override { return label_; }
  bool enabled(const Lighting& lighting) const override { return get_(lighting); }
  void set_enabled(Lighting* lighting, bool on) const override { set_(lighting, on); }
};

std::shared_ptr<LightingModule> fn(const char* id, const char* label, bool (*get)(const Lighting&),
                                  void (*set)(Lighting*, bool)) {
  auto m = std::make_shared<FnModule>();
  m->id_ = id;
  m->label_ = label;
  m->get_ = get;
  m->set_ = set;
  return m;
}
}  // namespace

void register_lighting_module(std::shared_ptr<LightingModule> module) {
  if (!module) return;
  for (const auto& m : modules()) {
    if (std::string(m->id()) == module->id()) return;
  }
  modules().push_back(std::move(module));
}

const std::vector<std::shared_ptr<LightingModule>>& lighting_modules() {
  if (modules().empty()) register_builtin_lighting();
  return modules();
}

void register_builtin_lighting() {
  if (!modules().empty()) return;
  register_lighting_module(fn("shadows", "Shadows",
                              [](const Lighting& l) { return l.cascades > 0; },
                              [](Lighting* l, bool on) { l->cascades = on ? (l->cascades > 0 ? l->cascades : 1) : 0; }));
  register_lighting_module(fn("pcss", "PCSS",
                              [](const Lighting& l) { return l.filter == 1; },
                              [](Lighting* l, bool on) { l->filter = on ? 1 : 0; }));
  register_lighting_module(fn("gi", "Global illumination",
                              [](const Lighting& l) { return l.gi != 0; },
                              [](Lighting* l, bool on) { if (!on) l->gi = 0; else if (l->gi == 0) l->gi = 1; }));
  register_lighting_module(fn("ao", "Ambient occlusion",
                              [](const Lighting& l) { return l.ao != 0; },
                              [](Lighting* l, bool on) { if (!on) l->ao = 0; else if (l->ao == 0) l->ao = 1; }));
  register_lighting_module(fn("reflections", "Reflections",
                              [](const Lighting& l) { return l.reflections != 0; },
                              [](Lighting* l, bool on) { if (!on) l->reflections = 0; else if (l->reflections == 0) l->reflections = 1; }));
  register_lighting_module(fn("bloom", "Bloom",
                              [](const Lighting& l) { return l.bloom > 0.0f; },
                              [](Lighting* l, bool on) { l->bloom = on ? (l->bloom > 0 ? l->bloom : 0.2f) : 0; }));
  register_lighting_module(fn("pom", "Parallax occlusion",
                              [](const Lighting& l) { return l.pom > 0.0f; },
                              [](Lighting* l, bool on) { l->pom = on ? (l->pom > 0 ? l->pom : 0.04f) : 0; }));
  register_lighting_module(fn("raytracing", "Hardware ray tracing",
                              [](const Lighting& l) { return l.raytracing; },
                              [](Lighting* l, bool on) { l->raytracing = on; }));
}

const char* lighting_preset_name(int preset) {
  switch (preset) {
    case 1: return "Fast";
    case 2: return "Balanced";
    case 3: return "Cinematic";
    case 4: return "Custom";
    default: return "Classic";
  }
}

void apply_lighting_preset(Lighting* lighting, int preset) {
  if (!lighting) return;
  Lighting next;
  next.preset = preset;
  if (preset == 1) {
    next.cascades = 1;
    next.filter = 0;
  } else if (preset == 2) {
    next.cascades = 2;
    next.filter = 0;
    next.ao = 1;
    next.bloom = 0.18f;
    next.tonemap = 1;
    next.gi = 1;
  } else if (preset == 3) {
    next.cascades = 4;
    next.filter = 1;
    next.ao = 1;
    next.reflections = 2;
    next.bloom = 0.35f;
    next.tonemap = 2;
    next.exposure = 1.05f;
    next.pom = 0.03f;
    next.contrast = 1.06f;
    next.saturation = 1.05f;
    next.spot_shadows = true;
  } else if (preset == 4) {
    next = *lighting;
    next.preset = 4;
  }
  *lighting = next;
}

bool lighting_is_classic(const Lighting& lighting) {
  Lighting classic;
  classic.hdri = lighting.hdri;
  classic.modules = lighting.modules;
  Lighting copy = lighting;
  copy.hdri.clear();
  copy.modules.clear();
  return copy == classic && lighting.preset == 0;
}

bool module_enabled(const Lighting& lighting, const char* id, bool fallback) {
  const std::string key = std::string(id) + "=";
  const std::size_t at = lighting.modules.find(key);
  if (at == std::string::npos) return fallback;
  return lighting.modules.compare(at + key.size(), 1, "1") == 0;
}

void set_module_enabled(Lighting* lighting, const char* id, bool on) {
  if (!lighting) return;
  const std::string key = std::string(id) + "=";
  const std::size_t at = lighting->modules.find(key);
  const std::string bit = on ? "1" : "0";
  if (at == std::string::npos) {
    if (!lighting->modules.empty()) lighting->modules += ",";
    lighting->modules += key + bit;
    return;
  }
  lighting->modules[at + key.size()] = bit[0];
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
