// Which split plugins a claim's content uses (Split.h: required_plugins).
// A light JSON walk over the scene files rather than the full scene
// loaders, so ts_server (no 3D runtime) can run it too.

#include "plugins/split/Split.h"

#include "plugins/RuntimePlugins.h"
#include "scene/JsonTree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace plugins {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool skipped_dir(const std::string& name) {
  if (name.empty() || name[0] == '.') return true;
  static const char* kSkip[] = {"export", "exports", "build", "builds", "backup", "autosave",
                                "plugins", "server", "thumbnails", "node_modules"};
  const std::string n = lower(name);
  for (const char* s : kSkip) {
    if (n == s) return true;
  }
  return false;
}

bool scene_like(const std::string& file) {
  const std::string n = lower(file);
  auto ends = [&](const char* suf) {
    const std::string s(suf);
    return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0;
  };
  return n == "scene.json" || ends(".scene.json") || ends(".scene3d.json") ||
         ends(".prefab.json") || ends(".tsscene");
}

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Scan {
  plugin::ProjectType type;
  std::set<std::string> need;
  std::vector<std::string>* why = nullptr;
  void add(const char* id, const std::string& reason) {
    if (need.insert(id).second && why) why->push_back(std::string(id) + ": " + reason);
  }
};

// One entity object (any nesting: prefabs keep entities under "entities"
// or "root"; children arrays are walked too).
void scan_entity(const json::Value& e, const std::string& file, Scan& s) {
  if (!e.is_object()) return;
  const std::string name = e["name"].as_string_or("entity");
  const std::string where = "'" + name + "' in " + file;
  const bool three_d = s.type != plugin::ProjectType::TwoD;
  if (three_d) {
    if (e.has("player") && e["player"].is_object()) {
      s.add(kRider3DId, "Player on " + where);
      s.add(kJoltId, "Player (character sweep) on " + where);
    }
    if (e.has("vr_rig") && e["vr_rig"].is_object()) {
      s.add(kJoltId, "VR Rig (grab / throw) on " + where);
      s.add(kRider3DId, "VR Rig rides flat on the desktop: " + where);
    }
    if (e["rigid_body"].is_object()) s.add(kJoltId, "Rigid Body on " + where);
    if (e["grabbable"].is_object()) s.add(kJoltId, "Grabbable on " + where);
    if (e["joint"].is_object()) s.add(kJoltId, "Joint on " + where);
  } else {
    if (e.has("player") && e["player"].is_object()) {
      if (e["player"]["mode"].as_string_or("") == "platformer") {
        s.add(kPlatformerId, "platformer Player on " + where);
      } else {
        s.add(kTopDownId, "top-down Player on " + where);
      }
    }
    if (e["body"].is_object()) s.add(kBox2DId, "Rigid Body on " + where);
    if (e["joint"].is_object()) s.add(kBox2DId, "Joint on " + where);
  }
  for (const char* kids : {"children", "entities"}) {
    if (e[kids].is_array()) {
      for (const json::Value& c : e[kids].items()) scan_entity(c, file, s);
    }
  }
}

void scan_file(const fs::path& path, const std::string& rel, Scan& s) {
  json::Value root;
  std::string err;
  if (!json::parse(read_all(path), &root, &err) || !root.is_object()) return;
  // Shipyard sky settings ("hull_physics": "box2d").
  if (s.type == plugin::ProjectType::TwoD && root["hull_physics"].as_string_or("") == "box2d") {
    s.add(kBox2DId, "Shipyard Box2D hulls in " + rel);
  }
  if (!scene_like(path.filename().string())) return;
  for (const char* key : {"entities", "root"}) {
    const json::Value& v = root[key];
    if (v.is_array()) {
      for (const json::Value& e : v.items()) scan_entity(e, rel, s);
    } else if (v.is_object()) {
      scan_entity(v, rel, s);
    }
  }
}

}  // namespace

std::vector<std::string> required_plugins(const std::string& claim_dir, plugin::ProjectType type,
                                          std::vector<std::string>* why) {
  Scan s{type, {}, why};
  std::error_code ec;
  const fs::path root(claim_dir);
  if (claim_dir.empty() || !fs::is_directory(root, ec)) return {};
  for (auto it = fs::recursive_directory_iterator(root, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path& p = it->path();
    if (it->is_directory(ec)) {
      if (skipped_dir(p.filename().string())) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || lower(p.extension().string()) != ".json") {
      if (!(it->is_regular_file(ec) && lower(p.extension().string()) == ".tsscene")) continue;
    }
    const std::string fname = lower(p.filename().string());
    // Scenes, prefabs and Shipyard sky files (sky*.json / *.sky.json).
    const bool sky = fname.find("sky") != std::string::npos;
    if (!scene_like(fname) && !sky) continue;
    scan_file(p, fs::relative(p, root, ec).generic_string(), s);
  }
  std::vector<std::string> out;
  for (const std::string& id : split_plugin_ids()) {
    if (s.need.count(id)) out.push_back(id);
  }
  return out;
}

std::vector<std::string> required_plugins(const std::string& claim_dir,
                                          std::vector<std::string>* why) {
  return required_plugins(claim_dir, read_project_plugins(claim_dir).type, why);
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
