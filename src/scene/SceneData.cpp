#include "scene/SceneData.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <utility>

namespace ts {
namespace tombstone {

bool operator==(const Entity2D& a, const Entity2D& b) {
  return a.id == b.id && a.name == b.name && a.x == b.x && a.y == b.y &&
         a.w == b.w && a.h == b.h && a.color[0] == b.color[0] &&
         a.color[1] == b.color[1] && a.color[2] == b.color[2] &&
         a.color[3] == b.color[3] && a.layer == b.layer &&
         a.tilemap == b.tilemap && a.sprite == b.sprite &&
         a.player == b.player && a.camera == b.camera && a.spawn == b.spawn &&
         a.collider == b.collider && a.animator == b.animator &&
         a.script == b.script && a.rotation == b.rotation &&
         a.parent == b.parent && a.locked == b.locked && a.hidden == b.hidden &&
         a.audio == b.audio;
}

ScriptValue ScriptValue::of_bool(bool v) {
  ScriptValue out;
  out.type = Type::Bool;
  out.flag = v;
  return out;
}

ScriptValue ScriptValue::of_number(double v) {
  ScriptValue out;
  out.type = Type::Number;
  out.number = v;
  return out;
}

ScriptValue ScriptValue::of_text(std::string v) {
  ScriptValue out;
  out.type = Type::Text;
  out.text = std::move(v);
  return out;
}

const char* to_string(ScriptValue::Type type) {
  switch (type) {
    case ScriptValue::Type::Bool:
      return "bool";
    case ScriptValue::Type::Number:
      return "number";
    case ScriptValue::Type::Text:
      return "text";
  }
  return "number";
}

std::string describe(const ScriptValue& value) {
  switch (value.type) {
    case ScriptValue::Type::Bool:
      return value.flag ? "true" : "false";
    case ScriptValue::Type::Text:
      return value.text;
    case ScriptValue::Type::Number:
      break;
  }
  std::ostringstream ss;
  ss.precision(10);
  ss << value.number;
  return ss.str();
}

const ScriptProp* ScriptData::find(const std::string& name) const {
  for (const ScriptProp& p : props) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

void ScriptData::set(const std::string& name, ScriptValue value) {
  for (ScriptProp& p : props) {
    if (p.name == name) {
      p.value = std::move(value);
      return;
    }
  }
  props.push_back(ScriptProp{name, std::move(value)});
  normalize_script(*this);
}

bool ScriptData::erase(const std::string& name) {
  const auto it = std::find_if(props.begin(), props.end(),
                               [&](const ScriptProp& p) { return p.name == name; });
  if (it == props.end()) {
    return false;
  }
  props.erase(it);
  return true;
}

void normalize_script(ScriptData& s) {
  std::vector<ScriptProp> clean;
  clean.reserve(s.props.size());
  for (ScriptProp& p : s.props) {
    if (p.name.empty()) {
      continue;
    }
    bool dup = false;
    for (const ScriptProp& c : clean) {
      dup = dup || c.name == p.name;
    }
    if (dup) {
      continue;
    }
    // Keep only the field the type uses, so equal values compare equal.
    ScriptValue v;
    v.type = p.value.type;
    switch (p.value.type) {
      case ScriptValue::Type::Bool:
        v.flag = p.value.flag;
        break;
      case ScriptValue::Type::Number:
        v.number = std::isfinite(p.value.number) ? p.value.number : 0.0;
        break;
      case ScriptValue::Type::Text:
        v.text = std::move(p.value.text);
        if (v.text.size() > ScriptData::kMaxTextLength) {
          v.text.resize(ScriptData::kMaxTextLength);
        }
        break;
    }
    clean.push_back(ScriptProp{std::move(p.name), std::move(v)});
  }
  std::stable_sort(clean.begin(), clean.end(),
                   [](const ScriptProp& a, const ScriptProp& b) {
                     return a.name < b.name;
                   });
  if (clean.size() > ScriptData::kMaxProps) {
    clean.resize(ScriptData::kMaxProps);
  }
  s.props = std::move(clean);
}

void sync_tilemap_extent(Entity2D& e) {
  if (!e.tilemap) {
    return;
  }
  e.tilemap->normalize();
  e.w = static_cast<float>(e.tilemap->cols * e.tilemap->tile_size);
  e.h = static_cast<float>(e.tilemap->rows * e.tilemap->tile_size);
}

ColliderData default_collider(const Entity2D& e) {
  ColliderData c;
  c.w = std::clamp(e.w, ColliderData::kMinSize, ColliderData::kMaxSize);
  c.h = std::clamp(e.h, ColliderData::kMinSize, ColliderData::kMaxSize);
  c.dynamic = e.player.has_value();
  return c;
}

void normalize_audio(AudioSourceData& a) {
  static const char* const kBuses[] = {"master", "music", "sfx", "ui", "voice"};
  bool known = false;
  for (const char* b : kBuses) known = known || a.bus == b;
  if (!known) a.bus = "sfx";
  if (a.clip.size() > 512) a.clip.resize(512);
  auto fix = [](float v, float lo, float hi, float dflt) {
    return v >= lo && v <= hi ? v : (v > hi ? hi : (v >= lo ? v : dflt));
  };
  a.volume = fix(a.volume, 0.0f, AudioSourceData::kMaxVolume, 1.0f);
  a.pitch = fix(a.pitch, 0.1f, 4.0f, 1.0f);
  a.min_distance = fix(a.min_distance, 0.0f, 1.0e6f, 160.0f);
  a.max_distance = fix(a.max_distance, 0.0f, 1.0e6f, 900.0f);
  if (a.max_distance < a.min_distance) a.max_distance = a.min_distance;
}

void normalize_components(Entity2D& e) {
  if (e.audio) normalize_audio(*e.audio);
  if (e.player) {
    e.player->slot = std::clamp(e.player->slot, 0, kMaxPlayers - 1);
    if (!(e.player->speed >= PlayerControllerData::kMinSpeed)) {
      e.player->speed = PlayerControllerData::kMinSpeed;  // also catches NaN
    }
    e.player->speed =
        std::min(e.player->speed, PlayerControllerData::kMaxSpeed);
  }
  if (e.camera) {
    Camera2DData& c = *e.camera;
    if (!(c.smoothing >= 0.0f)) {
      c.smoothing = 0.0f;
    }
    c.smoothing = std::min(c.smoothing, 5.0f);
    if (!(c.zoom >= 0.05f)) {
      c.zoom = 0.05f;
    }
    c.zoom = std::min(c.zoom, 16.0f);
    c.bounds_w = std::max(1.0f, c.bounds_w);
    c.bounds_h = std::max(1.0f, c.bounds_h);
  }
  if (e.spawn) {
    e.spawn->slot = std::clamp(e.spawn->slot, 0, kMaxPlayers - 1);
  }
  if (e.collider) {
    ColliderData& c = *e.collider;
    auto finite_or = [](float v, float fallback) {
      return std::isfinite(v) ? v : fallback;
    };
    c.offset_x = std::clamp(finite_or(c.offset_x, 0.0f),
                            -ColliderData::kMaxSize, ColliderData::kMaxSize);
    c.offset_y = std::clamp(finite_or(c.offset_y, 0.0f),
                            -ColliderData::kMaxSize, ColliderData::kMaxSize);
    c.w = std::clamp(finite_or(c.w, ColliderData::kMinSize),
                     ColliderData::kMinSize, ColliderData::kMaxSize);
    c.h = std::clamp(finite_or(c.h, ColliderData::kMinSize),
                     ColliderData::kMinSize, ColliderData::kMaxSize);
  }
  if (e.animator) {
    AnimatorData& an = *e.animator;
    if (!(an.speed >= 0.0f)) {
      an.speed = an.speed < 0.0f ? 0.0f : 1.0f;  // NaN plays at normal speed
    }
    an.speed = std::min(an.speed, AnimatorData::kMaxSpeed);
  }
  if (e.script) {
    normalize_script(*e.script);
  }
}

const Entity2D* find_entity(const std::vector<Entity2D>& entities,
                            std::uint64_t id) {
  for (const Entity2D& e : entities) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

const Entity2D* find_entity_named(const std::vector<Entity2D>& entities,
                                  const std::string& name) {
  for (const Entity2D& e : entities) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

std::vector<std::size_t> draw_order(const std::vector<Entity2D>& entities) {
  std::vector<std::size_t> order(entities.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) {
                     if (entities[a].layer != entities[b].layer) {
                       return entities[a].layer < entities[b].layer;
                     }
                     return entities[a].id < entities[b].id;
                   });
  return order;
}

}  // namespace tombstone
}  // namespace ts
