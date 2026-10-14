#pragma once

// TombStone plugins: gameplay features (crafting, combat, a grid ship
// builder...) and engine parts (the posse, scripting extras) packaged as
// modules a project switches on or off.
//
// A plugin describes itself (PluginInfo: id, version, dependencies, which
// project types it fits) and, when loaded, registers what it adds through
// a Registrar: World systems and component names, Lua tables, asset types,
// editor panels and menu items. Lifecycle, in order:
//
//   on_load(ctx)       once, dependencies already loaded. Return false (with
//                      a reason) to refuse; the plugin is then skipped.
//   on_register(reg)   right after on_load: declare contributions.
//   on_play_begin(w)   each time a ride starts (editor Play, ts_game).
//   on_play_end()      when that ride stops.
//   on_unload()        reverse load order, before destruction.
//
// Everything a plugin calls on the host is virtual or header-only, so a
// dynamic plugin (.dll / .so, see PluginAbi.h) links nothing from the host.

#include "plugin/Version.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace ts {
namespace fx {
struct LightInstance;
}  // namespace fx
namespace tombstone {
namespace runtime {
class World;
struct DrawQuad;
struct WorldRect;
struct Aabb;
}  // namespace runtime

namespace plugin {

// Project types a plugin fits (bit mask).
enum ProjectTypeBits : std::uint32_t {
  kType2D = 1u << 0,
  kType3D = 1u << 1,
  kTypeVR = 1u << 2,
  kTypeAll = kType2D | kType3D | kTypeVR,
};

enum class ProjectType { TwoD, ThreeD, VR };

inline std::uint32_t type_bit(ProjectType t) {
  switch (t) {
    case ProjectType::ThreeD: return kType3D;
    case ProjectType::VR: return kTypeVR;
    case ProjectType::TwoD: break;
  }
  return kType2D;
}

inline const char* to_string(ProjectType t) {
  switch (t) {
    case ProjectType::ThreeD: return "3D";
    case ProjectType::VR: return "VR";
    case ProjectType::TwoD: break;
  }
  return "2D";
}

enum class PluginCategory { Gameplay, Engine, Editor, Sample };

inline const char* to_string(PluginCategory c) {
  switch (c) {
    case PluginCategory::Engine: return "Engine";
    case PluginCategory::Editor: return "Editor";
    case PluginCategory::Sample: return "Sample";
    case PluginCategory::Gameplay: break;
  }
  return "Gameplay";
}

struct Dependency {
  std::string id;
  Version min;            // same major, at least this
  bool optional = false;  // load after it when present; fine without it
};

struct PluginInfo {
  std::string id;           // "ts.tumbleweed" -- stable, lowercase, dotted
  std::string name;         // "Tumbleweeds"
  std::string description;  // one line for the Plugins panel
  std::string author;
  Version version;
  std::vector<Dependency> deps;
  PluginCategory category = PluginCategory::Gameplay;
  std::uint32_t project_types = kType2D;
  bool default_enabled = false;  // on for new (and pre-plugin) projects
};

// --- Contributions -------------------------------------------------------------

struct EditorPanel {
  std::string id;          // "posse.network"
  std::string title;       // ImGui window title ("Network")
  std::string menu_label;  // View menu entry ("Network (Posse)")
  // Open flag the shell toggles; null = the shell keeps one.
  bool* open = nullptr;
  std::function<void()> before_begin;  // e.g. SetNextWindowFocus
  std::function<void()> draw;          // inside Begin/End
};

struct EditorMenuItem {
  std::string menu;   // top-level menu: "Play", "Tools"...
  std::string label;  // "Play with Posse"; "-" = separator
  std::string shortcut;
  std::function<void()> action;
  std::function<bool()> enabled;  // null = always
  std::function<bool()> checked;  // null = not a toggle
  // Optional: draw the entry yourself (submenus, live labels). When set,
  // label/action/enabled/checked are ignored.
  std::function<void()> draw;
};

struct AssetType {
  std::string extension;  // ".tumble.json" (lower case, leading dot)
  std::string label;      // "Tumbleweed spawn table"
};

// What a plugin calls during on_register. Pure virtual: the host owns the
// storage, the plugin only describes.
class Registrar {
 public:
  virtual ~Registrar() = default;
  virtual const std::string& plugin_id() const = 0;

  virtual void add_component(const std::string& name,
                             const std::string& description) = 0;
  // Runs every fixed tick of every ride, after scripts, in load order.
  virtual void add_system(const std::string& name,
                          std::function<void(runtime::World&, double)> run) = 0;
  // Global `name` in every script sandbox; `push` leaves one value.
  virtual void add_lua_api(const std::string& name,
                           std::function<void(lua_State*)> push) = 0;
  virtual void add_asset_type(const AssetType& type) = 0;
  virtual void add_editor_panel(EditorPanel panel) = 0;
  virtual void add_menu_item(EditorMenuItem item) = 0;

  // Quads (world space) drawn under (over = false) or on top of the actors
  // in every host that draws the World (editor Play, ts_game, ts_client).
  using DrawFn = std::function<void(const runtime::World&, const runtime::WorldRect& view,
                                    float alpha, std::vector<runtime::DrawQuad>* out)>;
  virtual void add_draw_layer(const std::string& /*name*/, bool /*over*/, DrawFn /*draw*/) {}
  // Solid boxes the World's movement collides with (riders, bodies, and a
  // client's prediction alike).
  using SolidsFn = std::function<void(const runtime::World&, const runtime::Aabb& area,
                                      std::vector<runtime::Aabb>* out)>;
  virtual void add_blocker(const std::string& /*name*/, SolidsFn /*solids*/) {}
  // A network channel: messages between a posse server's world and its
  // clients' (World::net_post / net_deliver; ts_mp carries them).
  using ReceiveFn = std::function<void(runtime::World&, int from_slot,
                                       const std::vector<std::uint8_t>& bytes)>;
  virtual void add_net_channel(const std::string& /*name*/, ReceiveFn /*receive*/) {}
  // HUD quads (world space, like layers) drawn last: over the lightmap.
  virtual void add_hud_layer(const std::string& /*name*/, DrawFn /*draw*/) {}
  // 2D lighting: lights each frame, what blocks light (a grid of `cell`
  // px) and an ambient that switches lighting on when it returns true.
  using LightsFn = std::function<void(const runtime::World&, const runtime::WorldRect& view,
                                      std::vector<fx::LightInstance>* out)>;
  virtual void add_light_source(const std::string& /*name*/, LightsFn /*lights*/) {}
  using OccluderFn = std::function<bool(const runtime::World&, int cx, int cy)>;
  virtual void add_light_occluder(const std::string& /*name*/, float /*cell*/,
                                  OccluderFn /*solid*/) {}
  using AmbientFn = std::function<bool(const runtime::World&, float out[3])>;
  virtual void add_ambient(const std::string& /*name*/, AmbientFn /*ambient*/) {}
};

// Where the plugin is being loaded.
struct PluginContext {
  ProjectType project_type = ProjectType::TwoD;
  std::string project_dir;
  // "editor", "game", "server", "smoke".
  std::string host = "smoke";
  // Host object for editor plugins (the editor screen); null elsewhere.
  // Plugins that need it check `host` first.
  void* host_object = nullptr;
};

class Plugin {
 public:
  virtual ~Plugin() = default;
  virtual const PluginInfo& info() const = 0;

  virtual bool on_load(const PluginContext& /*ctx*/, std::string* /*error*/) {
    return true;
  }
  virtual void on_register(Registrar& reg) = 0;
  virtual void on_play_begin(runtime::World& /*world*/) {}
  virtual void on_play_end() {}
  virtual void on_unload() {}
};

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
