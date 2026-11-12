#pragma once

#include "render3d/Renderer3D.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

// A pass a project, a preset or a plugin can turn on. Built-ins are
// registered at startup. Plugins call register_lighting_module.
class LightingModule {
 public:
  virtual ~LightingModule() = default;
  virtual const char* id() const = 0;
  virtual const char* label() const = 0;
  virtual bool enabled(const Lighting& lighting) const = 0;
  virtual void set_enabled(Lighting* lighting, bool on) const = 0;
};

void register_builtin_lighting();
void register_lighting_module(std::shared_ptr<LightingModule> module);
const std::vector<std::shared_ptr<LightingModule>>& lighting_modules();

void apply_lighting_preset(Lighting* lighting, int preset);
const char* lighting_preset_name(int preset);
bool lighting_is_classic(const Lighting& lighting);

// "id=1,other=0" helpers for plugin modules.
bool module_enabled(const Lighting& lighting, const char* id, bool fallback);
void set_module_enabled(Lighting* lighting, const char* id, bool on);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
