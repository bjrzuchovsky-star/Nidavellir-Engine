#include "scene/SceneJson.h"

#include "core/JsonMini.h"
#include "scene/JsonFlat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace scene_json {

namespace fs = std::filesystem;

namespace {

using json_mini::escape_string;
using json_mini::match_char;
using json_mini::parse_bool;
using json_mini::parse_string;
using json_mini::skip_ws;
using namespace json_flat;

// Shortest text that reads back as the same double (script numbers).
std::string format_double(double v) {
  for (int precision = 15; precision <= 17; ++precision) {
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.precision(precision);
    ss << v;
    const std::string text = ss.str();
    if (std::strtod(text.c_str(), nullptr) == v) {
      return text;
    }
  }
  std::ostringstream ss;
  ss.imbue(std::locale::classic());
  ss.precision(17);
  ss << v;
  return ss.str();
}

// v6 "script": {"path": "...", "props": [{"name": "reach", "number": 24},
// {"name": "line", "text": "..."}, {"name": "locked", "bool": false}]}.
// Which value key is present gives the type.
bool parse_script(const std::string& raw, ScriptData* out) {
  std::size_t i = 0;
  FlatMap inner_nested;
  const auto obj = parse_flat_object(raw, i, &inner_nested);
  if (!obj) {
    return false;
  }
  out->path = map_string(*obj, "path", "");
  out->props.clear();
  const auto props = inner_nested.find("props");
  if (props == inner_nested.end()) {
    return true;
  }
  std::vector<std::string> items;
  if (!split_object_array(props->second, &items)) {
    return false;
  }
  for (const std::string& item : items) {
    std::size_t j = 0;
    const auto p = parse_flat_object(item, j);
    if (!p) {
      return false;
    }
    ScriptProp prop;
    prop.name = map_string(*p, "name", "");
    if (p->count("bool")) {
      prop.value = ScriptValue::of_bool(map_bool(*p, "bool", false));
    } else if (p->count("text")) {
      prop.value = ScriptValue::of_text(map_string(*p, "text", ""));
    } else if (p->count("number")) {
      prop.value = ScriptValue::of_number(map_number(*p, "number", 0.0));
    } else {
      return false;
    }
    out->props.push_back(std::move(prop));
  }
  return true;
}

Entity2D entity_from_map(const FlatMap& map) {
  Entity2D e;
  e.id = map_u64(map, "id", 0);
  e.name = map_string(map, "name", "Entity");
  e.x = map_float(map, "x", 0.0f);
  e.y = map_float(map, "y", 0.0f);
  e.w = map_float(map, "w", 64.0f);
  e.h = map_float(map, "h", 64.0f);
  e.color[0] = map_float(map, "r", map_float(map, "color_r", 0.35f));
  e.color[1] = map_float(map, "g", map_float(map, "color_g", 0.65f));
  e.color[2] = map_float(map, "b", map_float(map, "color_b", 0.95f));
  e.color[3] = map_float(map, "a", map_float(map, "color_a", 1.0f));
  e.layer = map_int(map, "layer", 0);
  // v7 editor keys; absent = defaults.
  e.rotation = map_float(map, "rotation", 0.0f);
  if (!std::isfinite(e.rotation)) e.rotation = 0.0f;
  e.parent = map_u64(map, "parent", 0);
  e.locked = map_bool(map, "locked", false);
  e.hidden = map_bool(map, "hidden", false);
  return e;
}

// Top-level v4 "tile_solidity": [{"tileset": "", "solid": [3, 5]}, ...].
// i sits on the '['.
bool parse_tile_solidity(std::string_view text, std::size_t& i,
                         TileSolidity* out) {
  out->overrides.clear();
  if (!match_char(text, i, '[')) {
    return false;
  }
  skip_ws(text, i);
  if (match_char(text, i, ']')) {
    return true;
  }
  while (true) {
    FlatMap nested;
    auto obj = parse_flat_object(text, i, &nested);
    if (!obj) {
      return false;
    }
    std::vector<int> ids;
    const auto arr = nested.find("solid");
    if (arr == nested.end() || !parse_int_array(arr->second, &ids)) {
      return false;
    }
    out->overrides[map_string(*obj, "tileset", "")] = std::move(ids);
    skip_ws(text, i);
    if (match_char(text, i, ']')) {
      break;
    }
    if (!match_char(text, i, ',')) {
      return false;
    }
  }
  out->normalize();
  return true;
}

// Components: v2 "tilemap" {cols, rows, tile_size, tileset, encoding, data}
// and "sprite" {path, flip_x, flip_y, use_src, src_x/src_y/src_w/src_h};
// v3 "player" {slot, speed}, "camera" {target, smoothing, zoom, use_bounds,
// bounds_x/y/w/h} and "spawn" {slot}; v4 "collider" {x, y, w, h,
// type: "solid"|"trigger", body: "static"|"dynamic"}; v5 "animator" {set,
// clip, default_clip, speed, playing}; v6 "script" {path, props}.
bool apply_components(Entity2D& e, const FlatMap& nested, std::string* why) {
  auto bad = [&](const char* what) {
    if (why) *why = what;
    return false;
  };
  if (auto tm = parse_nested(nested, "tilemap")) {
    TileMapData data;
    data.cols = map_int(*tm, "cols", 1);
    data.rows = map_int(*tm, "rows", 1);
    data.tile_size = map_int(*tm, "tile_size", 32);
    data.tileset = map_string(*tm, "tileset", "");
    data.normalize();
    const std::string encoding = map_string(*tm, "encoding", "rle");
    if (encoding != "rle") {
      if (why) *why = "unknown tilemap encoding '" + encoding + "'";
      return false;
    }
    if (!tile_codec::decode_rle(map_string(*tm, "data", ""),
                                data.tiles.size(), &data.tiles)) {
      return bad("bad tilemap data");
    }
    e.tilemap = std::move(data);
    sync_tilemap_extent(e);
  } else if (nested.count("tilemap")) {
    return bad("bad tilemap object");
  }
  if (auto sp = parse_nested(nested, "sprite")) {
    SpriteData s;
    s.path = map_string(*sp, "path", "");
    s.flip_x = map_bool(*sp, "flip_x", false);
    s.flip_y = map_bool(*sp, "flip_y", false);
    s.use_src_rect = map_bool(*sp, "use_src", false);
    s.src_x = map_int(*sp, "src_x", 0);
    s.src_y = map_int(*sp, "src_y", 0);
    s.src_w = map_int(*sp, "src_w", 0);
    s.src_h = map_int(*sp, "src_h", 0);
    e.sprite = std::move(s);
  } else if (nested.count("sprite")) {
    return bad("bad sprite object");
  }
  if (auto pl = parse_nested(nested, "player")) {
    PlayerControllerData p;
    p.slot = map_int(*pl, "slot", 0);
    p.speed = map_float(*pl, "speed", p.speed);
    e.player = p;
  } else if (nested.count("player")) {
    return bad("bad player object");
  }
  if (auto cm = parse_nested(nested, "camera")) {
    Camera2DData c;
    c.target = map_u64(*cm, "target", 0);
    c.smoothing = map_float(*cm, "smoothing", c.smoothing);
    c.zoom = map_float(*cm, "zoom", c.zoom);
    c.use_bounds = map_bool(*cm, "use_bounds", false);
    c.bounds_x = map_float(*cm, "bounds_x", c.bounds_x);
    c.bounds_y = map_float(*cm, "bounds_y", c.bounds_y);
    c.bounds_w = map_float(*cm, "bounds_w", c.bounds_w);
    c.bounds_h = map_float(*cm, "bounds_h", c.bounds_h);
    e.camera = c;
  } else if (nested.count("camera")) {
    return bad("bad camera object");
  }
  if (auto spn = parse_nested(nested, "spawn")) {
    SpawnPointData s;
    s.slot = map_int(*spn, "slot", 0);
    e.spawn = s;
  } else if (nested.count("spawn")) {
    return bad("bad spawn object");
  }
  if (auto co = parse_nested(nested, "collider")) {
    ColliderData c;
    c.offset_x = map_float(*co, "x", 0.0f);
    c.offset_y = map_float(*co, "y", 0.0f);
    c.w = map_float(*co, "w", e.w);
    c.h = map_float(*co, "h", e.h);
    const std::string type = map_string(*co, "type", "solid");
    const std::string body = map_string(*co, "body", "static");
    if ((type != "solid" && type != "trigger") ||
        (body != "static" && body != "dynamic")) {
      return bad("bad collider type/body");
    }
    c.trigger = type == "trigger";
    c.dynamic = body == "dynamic";
    e.collider = c;
  } else if (nested.count("collider")) {
    return bad("bad collider object");
  }
  if (auto an = parse_nested(nested, "animator")) {
    AnimatorData a;
    a.set = map_string(*an, "set", "");
    a.clip = map_string(*an, "clip", "");
    a.default_clip = map_string(*an, "default_clip", "");
    a.speed = map_float(*an, "speed", 1.0f);
    a.playing = map_bool(*an, "playing", true);
    e.animator = std::move(a);
  } else if (nested.count("animator")) {
    return bad("bad animator object");
  }
  if (const auto sc = nested.find("script"); sc != nested.end()) {
    ScriptData data;
    if (!parse_script(sc->second, &data)) {
      return bad("bad script object");
    }
    e.script = std::move(data);
  }
  if (auto au = parse_nested(nested, "audio")) {
    AudioSourceData a;
    a.clip = map_string(*au, "clip", "");
    a.bus = map_string(*au, "bus", "sfx");
    a.volume = map_float(*au, "volume", 1.0f);
    a.pitch = map_float(*au, "pitch", 1.0f);
    a.loop = map_bool(*au, "loop", true);
    a.autoplay = map_bool(*au, "autoplay", true);
    a.spatial = map_bool(*au, "spatial", true);
    a.min_distance = map_float(*au, "min_distance", a.min_distance);
    a.max_distance = map_float(*au, "max_distance", a.max_distance);
    a.doppler = map_bool(*au, "doppler", false);
    e.audio = std::move(a);
  } else if (nested.count("audio")) {
    return bad("bad audio object");
  }
  normalize_components(e);
  return true;
}

// Version 1 scenes stored the seeded TileMap as a plain rect. Give any
// "TileMap*" entity an empty 32 px grid covering its old rect so it can be
// painted right away.
void upgrade_v1_tilemaps(std::vector<Entity2D>& entities) {
  for (Entity2D& e : entities) {
    if (e.tilemap || e.name.rfind("TileMap", 0) != 0) {
      continue;
    }
    const int cols = std::max(1, static_cast<int>(std::lround(e.w / 32.0f)));
    const int rows = std::max(1, static_cast<int>(std::lround(e.h / 32.0f)));
    e.tilemap = TileMapData(cols, rows, 32);
    sync_tilemap_extent(e);
  }
}

// Version 1-2 scenes predate gameplay components. The seeded placeholders
// get what a fresh v3 scene has, so Play works on old projects: "Player"
// rides slot 0 and "Camera2D" follows it. Nothing else is touched.
void upgrade_v2_actors(std::vector<Entity2D>& entities) {
  for (const Entity2D& e : entities) {
    if (e.has_gameplay()) {
      return;  // somebody already wired gameplay by hand
    }
  }
  Entity2D* player = nullptr;
  for (Entity2D& e : entities) {
    if (e.name == "Player" && !e.tilemap) {
      player = &e;
      break;
    }
  }
  if (player) {
    player->player = PlayerControllerData{};
  }
  for (Entity2D& e : entities) {
    if (e.name == "Camera2D" && !e.tilemap) {
      Camera2DData cam;
      cam.target = player ? player->id : 0;
      e.camera = cam;
      break;
    }
  }
}

// Version 1-3 scenes predate collision: every rider gets the default
// dynamic collider (its whole rect) so old projects still bump into walls.
void upgrade_v3_colliders(std::vector<Entity2D>& entities) {
  for (Entity2D& e : entities) {
    if (e.player && !e.collider) {
      e.collider = default_collider(e);
    }
  }
}

void write_entity(std::ostringstream& out, const Entity2D& e) {
  out << "    {\n";
  out << "      \"id\": " << static_cast<unsigned long long>(e.id) << ",\n";
  out << "      \"name\": \"" << escape_string(e.name) << "\",\n";
  out << "      \"x\": " << format_float(e.x) << ",\n";
  out << "      \"y\": " << format_float(e.y) << ",\n";
  out << "      \"w\": " << format_float(e.w) << ",\n";
  out << "      \"h\": " << format_float(e.h) << ",\n";
  out << "      \"r\": " << format_float(e.color[0]) << ",\n";
  out << "      \"g\": " << format_float(e.color[1]) << ",\n";
  out << "      \"b\": " << format_float(e.color[2]) << ",\n";
  out << "      \"a\": " << format_float(e.color[3]) << ",\n";
  out << "      \"layer\": " << e.layer;
  // v7: only written when set, so untouched entities read like v6.
  if (e.rotation != 0.0f) out << ",\n      \"rotation\": " << format_float(e.rotation);
  if (e.parent != 0) {
    out << ",\n      \"parent\": " << static_cast<unsigned long long>(e.parent);
  }
  if (e.locked) out << ",\n      \"locked\": true";
  if (e.hidden) out << ",\n      \"hidden\": true";
  if (e.tilemap) {
    const TileMapData& tm = *e.tilemap;
    out << ",\n      \"tilemap\": {\"cols\": " << tm.cols
        << ", \"rows\": " << tm.rows << ", \"tile_size\": " << tm.tile_size
        << ", \"tileset\": \"" << escape_string(tm.tileset)
        << "\", \"encoding\": \"rle\", \"data\": \""
        << tile_codec::encode_rle(tm.tiles) << "\"}";
  }
  if (e.sprite) {
    const SpriteData& sp = *e.sprite;
    out << ",\n      \"sprite\": {\"path\": \"" << escape_string(sp.path)
        << "\", \"flip_x\": " << (sp.flip_x ? "true" : "false")
        << ", \"flip_y\": " << (sp.flip_y ? "true" : "false")
        << ", \"use_src\": " << (sp.use_src_rect ? "true" : "false")
        << ", \"src_x\": " << sp.src_x << ", \"src_y\": " << sp.src_y
        << ", \"src_w\": " << sp.src_w << ", \"src_h\": " << sp.src_h << "}";
  }
  if (e.player) {
    out << ",\n      \"player\": {\"slot\": " << e.player->slot
        << ", \"speed\": " << format_float(e.player->speed) << "}";
  }
  if (e.camera) {
    const Camera2DData& c = *e.camera;
    out << ",\n      \"camera\": {\"target\": "
        << static_cast<unsigned long long>(c.target)
        << ", \"smoothing\": " << format_float(c.smoothing)
        << ", \"zoom\": " << format_float(c.zoom)
        << ", \"use_bounds\": " << (c.use_bounds ? "true" : "false")
        << ", \"bounds_x\": " << format_float(c.bounds_x)
        << ", \"bounds_y\": " << format_float(c.bounds_y)
        << ", \"bounds_w\": " << format_float(c.bounds_w)
        << ", \"bounds_h\": " << format_float(c.bounds_h) << "}";
  }
  if (e.spawn) {
    out << ",\n      \"spawn\": {\"slot\": " << e.spawn->slot << "}";
  }
  if (e.collider) {
    const ColliderData& c = *e.collider;
    out << ",\n      \"collider\": {\"x\": " << format_float(c.offset_x)
        << ", \"y\": " << format_float(c.offset_y)
        << ", \"w\": " << format_float(c.w)
        << ", \"h\": " << format_float(c.h) << ", \"type\": \""
        << (c.trigger ? "trigger" : "solid") << "\", \"body\": \""
        << (c.dynamic ? "dynamic" : "static") << "\"}";
  }
  if (e.animator) {
    const AnimatorData& a = *e.animator;
    out << ",\n      \"animator\": {\"set\": \"" << escape_string(a.set)
        << "\", \"clip\": \"" << escape_string(a.clip)
        << "\", \"default_clip\": \"" << escape_string(a.default_clip)
        << "\", \"speed\": " << format_float(a.speed)
        << ", \"playing\": " << (a.playing ? "true" : "false") << "}";
  }
  if (e.script) {
    const ScriptData& sc = *e.script;
    out << ",\n      \"script\": {\"path\": \"" << escape_string(sc.path)
        << "\", \"props\": [";
    for (std::size_t k = 0; k < sc.props.size(); ++k) {
      const ScriptProp& p = sc.props[k];
      out << (k > 0 ? ", " : "") << "{\"name\": \"" << escape_string(p.name)
          << "\", ";
      switch (p.value.type) {
        case ScriptValue::Type::Bool:
          out << "\"bool\": " << (p.value.flag ? "true" : "false");
          break;
        case ScriptValue::Type::Number:
          out << "\"number\": " << format_double(p.value.number);
          break;
        case ScriptValue::Type::Text:
          out << "\"text\": \"" << escape_string(p.value.text) << "\"";
          break;
      }
      out << "}";
    }
    out << "]}";
  }
  if (e.audio) {
    const AudioSourceData& a = *e.audio;
    out << ",\n      \"audio\": {\"clip\": \"" << escape_string(a.clip)
        << "\", \"bus\": \"" << escape_string(a.bus)
        << "\", \"volume\": " << format_float(a.volume)
        << ", \"pitch\": " << format_float(a.pitch)
        << ", \"loop\": " << (a.loop ? "true" : "false")
        << ", \"autoplay\": " << (a.autoplay ? "true" : "false")
        << ", \"spatial\": " << (a.spatial ? "true" : "false")
        << ", \"min_distance\": " << format_float(a.min_distance)
        << ", \"max_distance\": " << format_float(a.max_distance)
        << ", \"doppler\": " << (a.doppler ? "true" : "false") << "}";
  }
  out << "\n";
  out << "    }";
}

}  // namespace

std::string scene_path_for_project(const std::string& project_dir) {
  if (project_dir.empty()) {
    return "scene.json";
  }
  if (project_dir.back() == '/' || project_dir.back() == '\\') {
    return project_dir + "scene.json";
  }
  return project_dir + "/scene.json";
}

void repair_parents(std::vector<Entity2D>& entities) {
  auto find_parent = [&](std::uint64_t id) -> const Entity2D* {
    for (const Entity2D& e : entities) {
      if (e.id == id) return &e;
    }
    return nullptr;
  };
  for (Entity2D& e : entities) {
    if (e.parent == 0) continue;
    // Walk up; a chain longer than the scene is a loop.
    std::uint64_t at = e.parent;
    std::size_t steps = 0;
    bool ok = true;
    while (at != 0) {
      const Entity2D* p = find_parent(at);
      if (!p || at == e.id || ++steps > entities.size()) {
        ok = false;
        break;
      }
      at = p->parent;
    }
    if (!ok) e.parent = 0;
  }
}

bool parse(const std::string& text, SceneDoc* doc, std::string* error_out,
           const std::string& source) {
  auto fail = [&](const std::string& what) {
    if (error_out) {
      *error_out = "Invalid scene.json (" + what + "): " + source;
    }
    return false;
  };
  SceneDoc out;
  out.version = 1;  // files without "version" predate v2
  std::size_t i = 0;
  if (!match_char(text, i, '{')) {
    return fail("expected object");
  }
  bool saw_entities = false;
  skip_ws(text, i);
  if (match_char(text, i, '}')) {
    // "{}" is an empty v1 scene.
    *doc = std::move(out);
    return true;
  }
  while (true) {
    auto key = parse_string(text, i);
    if (!key || !match_char(text, i, ':')) {
      return fail("bad key");
    }
    skip_ws(text, i);
    if (*key == "entities") {
      if (!match_char(text, i, '[')) {
        return fail("entities must be array");
      }
      saw_entities = true;
      skip_ws(text, i);
      if (!match_char(text, i, ']')) {
        while (true) {
          FlatMap nested;
          auto obj = parse_flat_object(text, i, &nested);
          if (!obj) {
            return fail("bad entity object");
          }
          Entity2D e = entity_from_map(*obj);
          if (e.id == 0 || e.name.empty()) {
            return fail("entity missing id/name");
          }
          std::string why;
          if (!apply_components(e, nested, &why)) {
            return fail("entity " + e.name + ": " + why);
          }
          out.entities.push_back(std::move(e));
          skip_ws(text, i);
          if (match_char(text, i, ']')) {
            break;
          }
          if (!match_char(text, i, ',')) {
            return fail("entities array");
          }
        }
      }
    } else if (*key == "pan_x" || *key == "pan_y" || *key == "zoom" ||
               *key == "grid_size" || *key == "version") {
      const std::size_t start = i;
      auto n = parse_number(text, i);
      if (!n) {
        return fail(*key);
      }
      const std::string num(text.substr(start, i - start));
      const float f = std::strtof(num.c_str(), nullptr);
      if (*key == "pan_x") {
        out.pan_x = f;
      } else if (*key == "pan_y") {
        out.pan_y = f;
      } else if (*key == "zoom") {
        out.zoom = f;
      } else if (*key == "grid_size") {
        out.grid_size = f;
      } else {
        out.version = static_cast<int>(*n);
      }
    } else if (*key == "show_grid" || *key == "snap") {
      auto b = parse_bool(text, i);
      if (!b) {
        return fail(*key);
      }
      if (*key == "show_grid") {
        out.show_grid = *b;
      } else {
        out.snap = *b;
      }
    } else if (*key == "selection") {
      if (!match_char(text, i, '[')) {
        return fail("selection must be array");
      }
      skip_ws(text, i);
      if (!match_char(text, i, ']')) {
        while (true) {
          skip_ws(text, i);
          auto n = parse_number(text, i);
          if (!n || *n < 0.0) {
            return fail("selection");
          }
          out.selection.push_back(static_cast<std::uint64_t>(*n + 0.5));
          skip_ws(text, i);
          if (match_char(text, i, ']')) {
            break;
          }
          if (!match_char(text, i, ',')) {
            return fail("selection");
          }
        }
      }
    } else if (*key == "dimension") {
      // v7+ 3D scenes carry "dimension": "3d" (render3d/Scene3D.h); this
      // reader only knows the 2D formats.
      auto dim = parse_string(text, i);
      if (!dim) {
        return fail("dimension");
      }
      if (*dim != "2d") {
        return fail("a " + std::string(*dim == "3d" ? "3D" : *dim) +
                    " scene; open it in a 3D claim");
      }
    } else if (*key == "tile_solidity") {
      if (!parse_tile_solidity(text, i, &out.tile_solidity)) {
        return fail("tile_solidity");
      }
    } else if (*key == "selected_id") {
      skip_ws(text, i);
      if (i < text.size() && text[i] == 'n') {
        if (text.compare(i, 4, "null") != 0) {
          return fail("selected_id");
        }
        i += 4;
        out.selected_id.reset();
      } else {
        auto n = parse_number(text, i);
        if (!n) {
          return fail("selected_id");
        }
        out.selected_id = static_cast<std::uint64_t>(*n + 0.5);
      }
    } else {
      // Unknown keys are skipped so newer files still open.
      skip_ws(text, i);
      if (i < text.size() && text[i] == '"') {
        if (!parse_string(text, i)) {
          return fail("unknown value");
        }
      } else if (parse_bool(text, i)) {
      } else if (parse_number(text, i)) {
      } else if (i < text.size() && (text[i] == '[' || text[i] == '{')) {
        if (!skip_bracketed(text, i)) {
          return fail("unknown value");
        }
      } else if (text.compare(i, 4, "null") == 0) {
        i += 4;
      } else {
        return fail("unknown value");
      }
    }
    skip_ws(text, i);
    if (match_char(text, i, '}')) {
      break;
    }
    if (!match_char(text, i, ',')) {
      return fail("trailing");
    }
  }
  if (!saw_entities) {
    return fail("missing entities");
  }
  if (out.version < 2) {
    upgrade_v1_tilemaps(out.entities);
  }
  if (out.version < 3) {
    upgrade_v2_actors(out.entities);
  }
  if (out.version < 4) {
    upgrade_v3_colliders(out.entities);
  }
  // v7: a parent that is missing, itself, or part of a loop goes back to
  // the root (hand-edited files).
  repair_parents(out.entities);
  *doc = std::move(out);
  return true;
}

std::string write(const SceneDoc& doc) {
  std::ostringstream out;
  out << "{\n";
  // v8: optional per-entity "audio" (sound source); v7 files load as-is.
  // v7: optional per-entity "rotation" / "parent" / "locked" / "hidden".
  // v6: optional per-entity "script"; v5 added "animator", v4 "collider"
  // plus the scene-level tile_solidity table, v3 "player" / "camera" /
  // "spawn" and v2 "tilemap" / "sprite". Older files still load; see
  // parse(). v4 -> v5 -> v6 need no upgrade: a scene without animators or
  // scripts simply has none.
  out << "  \"version\": " << kSceneVersion << ",\n";
  out << "  \"pan_x\": " << format_float(doc.pan_x) << ",\n";
  out << "  \"pan_y\": " << format_float(doc.pan_y) << ",\n";
  out << "  \"zoom\": " << format_float(doc.zoom) << ",\n";
  out << "  \"show_grid\": " << (doc.show_grid ? "true" : "false") << ",\n";
  out << "  \"grid_size\": " << format_float(doc.grid_size.value_or(32.0f))
      << ",\n";
  out << "  \"snap\": " << (doc.snap.value_or(false) ? "true" : "false")
      << ",\n";
  if (doc.selected_id) {
    out << "  \"selected_id\": "
        << static_cast<unsigned long long>(*doc.selected_id) << ",\n";
  } else {
    out << "  \"selected_id\": null,\n";
  }
  out << "  \"selection\": [";
  for (std::size_t i = 0; i < doc.selection.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << static_cast<unsigned long long>(doc.selection[i]);
  }
  out << "],\n";
  TileSolidity solidity = doc.tile_solidity;
  solidity.normalize();
  if (solidity.overrides.empty()) {
    out << "  \"tile_solidity\": [],\n";
  } else {
    out << "  \"tile_solidity\": [\n";
    std::size_t n = 0;
    for (const auto& [tileset, ids] : solidity.overrides) {
      out << "    {\"tileset\": \"" << escape_string(tileset)
          << "\", \"solid\": [";
      for (std::size_t k = 0; k < ids.size(); ++k) {
        out << (k > 0 ? ", " : "") << ids[k];
      }
      out << "]}" << (++n < solidity.overrides.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
  }
  out << "  \"entities\": [\n";
  for (std::size_t idx = 0; idx < doc.entities.size(); ++idx) {
    write_entity(out, doc.entities[idx]);
    if (idx + 1 < doc.entities.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

bool load_file(const std::string& path, SceneDoc* doc, std::string* error_out) {
  if (path.empty()) {
    if (error_out) *error_out = "scene path is empty";
    return false;
  }
  std::error_code ec;
  if (!fs::exists(fs::path(path), ec)) {
    if (error_out) *error_out = "Missing scene.json: " + path;
    return false;
  }
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    if (error_out) *error_out = "Could not open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), doc, error_out, path);
}

bool save_file(const std::string& path, const SceneDoc& doc,
               std::string* error_out) {
  if (path.empty()) {
    if (error_out) *error_out = "scene path is empty";
    return false;
  }
  const fs::path p = fs::path(path);
  std::error_code ec;
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
      if (error_out) {
        *error_out =
            "Could not create directory for " + path + ": " + ec.message();
      }
      return false;
    }
  }
  const std::string text = write(doc);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error_out) *error_out = "Could not write " + path;
    return false;
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    if (error_out) *error_out = "Failed while writing " + path;
    return false;
  }
  return true;
}

}  // namespace scene_json
}  // namespace tombstone
}  // namespace ts
