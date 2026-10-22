#include "editor/templates/TemplateStore.h"

#include "editor/launch/GameLauncher.h"
#include "editor/templates/EmptyTemplates.h"
#include "editor/projects/ProjectStore.h"
#include "editor/workspace/SceneIO.h"
#include "editor/workspace/Workspace2D.h"
#include "plugins/shipyard/Sample.h"
#include "plugins/survival/Sample.h"
#include "render3d/Scene3D.h"
#include "scene/SampleRider.h"
#include "scene/SampleScripts.h"
#include "store/Json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

std::string now_utc() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm_buf{};
#if defined(_WIN32)
  gmtime_s(&tm_buf, &t);
#else
  gmtime_r(&t, &tm_buf);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Editor-only clutter that never goes into (or comes out of) a template.
bool skip_entry(const fs::path& name) {
  const std::string n = name.string();
  if (n.empty() || n[0] == '.') return true;
  return n == "imgui.ini" || n == "export" || n == "Export" || n == "depot" ||
         n == "build" || n == "Builds";
}

bool copy_tree(const fs::path& from, const fs::path& to, std::string* error) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) {
    if (error) *error = "could not create " + to.string() + ": " + ec.message();
    return false;
  }
  for (const auto& e : fs::directory_iterator(from, ec)) {
    if (skip_entry(e.path().filename())) continue;
    const fs::path dst = to / e.path().filename();
    if (e.is_directory(ec)) {
      if (!copy_tree(e.path(), dst, error)) return false;
    } else if (e.is_regular_file(ec)) {
      fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        if (error) *error = "could not copy " + e.path().string() + ": " + ec.message();
        return false;
      }
    }
  }
  if (ec) {
    if (error) *error = "could not read " + from.string() + ": " + ec.message();
    return false;
  }
  return true;
}

// Rewrites project.json for a freshly stamped claim, keeping any fields a
// sample added that this code does not know about.
bool stamp_project_json(const fs::path& dir, const std::string& name, ProjectKind kind,
                        bool vr, const std::vector<std::string>* plugins, std::string* error) {
  const fs::path file = dir / "project.json";
  store::Json j = store::Json::object();
  std::error_code ec;
  if (fs::exists(file, ec)) {
    std::string err;
    if (!store::read_json_file(file.string(), &j, &err) || !j.is_object()) {
      if (error) *error = "template project.json is unreadable: " + err;
      return false;
    }
  }
  store::Json out = store::Json::object();
  out.set("name", name);
  out.set("dimension", dimension_string(kind));
  out.set("created", now_utc());
  if (kind != ProjectKind::TwoD) out.set("vr", vr);
  for (const auto& m : j.members()) {
    if (m.first == "name" || m.first == "dimension" || m.first == "created" ||
        m.first == "last_opened" || m.first == "plugins" || m.first == "vr" || m.first == "xr") {
      continue;
    }
    out.set(m.first, m.second);
  }
  if (plugins) {
    out.set("plugins", ProjectStore::join_plugin_ids(*plugins));
  } else if (j.has("plugins")) {
    out.set("plugins", j.get("plugins"));
  }
  return store::write_text_file(file.string(), out.dump(), error);
}

void add_unique(std::vector<std::string>& v, const std::string& s) {
  if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

}  // namespace

std::vector<std::string> TemplateStore::engine_template_dirs() {
  std::vector<std::string> dirs;
  if (const char* env = std::getenv("TS_TEMPLATES_PATH")) {
    std::string cur;
    for (const char* c = env;; ++c) {
      if (*c == ';' || *c == '\0'
#if !defined(_WIN32)
          || *c == ':'
#endif
      ) {
        add_unique(dirs, cur);
        cur.clear();
        if (*c == '\0') break;
      } else {
        cur += *c;
      }
    }
  }
  const std::string self = launcher::self_executable_path();
  if (!self.empty()) {
    std::error_code ec;
    const fs::path next_to_exe = fs::path(self).parent_path() / "templates";
    if (fs::is_directory(next_to_exe, ec)) add_unique(dirs, next_to_exe.string());
  }
#if defined(TS_TEMPLATES_SOURCE_DIR)
  {
    std::error_code ec;
    if (fs::is_directory(TS_TEMPLATES_SOURCE_DIR, ec)) add_unique(dirs, TS_TEMPLATES_SOURCE_DIR);
  }
#endif
  return dirs;
}

std::string TemplateStore::user_template_dir(const std::string& projects_root) {
  if (projects_root.empty()) return {};
  return (fs::path(projects_root) / ".templates").string();
}

bool TemplateStore::load_template(const std::string& dir, bool user, ProjectTemplate* out,
                                  std::string* error) {
  const fs::path d(dir);
  store::Json j;
  if (!store::read_json_file((d / "template.json").string(), &j, error)) return false;
  if (!j.is_object()) {
    if (error) *error = (d / "template.json").string() + ": not an object";
    return false;
  }
  ProjectTemplate t;
  t.dir = d.string();
  t.user = user;
  t.id = j.string_at("id", d.filename().string());
  t.name = j.string_at("name", t.id);
  t.description = j.string_at("description");
  t.kind = project_kind_from_dimension(j.string_at("dimension", "2d"));
  t.vr = j.get("vr").as_bool(false) || j.string_at("vr") == "true";
  if (t.kind == ProjectKind::VR) {  // legacy "dimension": "vr"
    t.kind = ProjectKind::ThreeD;
    t.vr = true;
  }
  if (t.kind == ProjectKind::TwoD) t.vr = false;
  if (j.has("plugins")) {
    t.plugins_set = true;
    t.plugins = j.strings_at("plugins");
  }
  t.tags = j.strings_at("tags");
  t.order = static_cast<int>(j.get("order").as_number(user ? 5000 : 1000));
  t.generator = j.string_at("generator");
  std::error_code ec;
  const std::string thumb = j.string_at("thumbnail", "thumbnail.png");
  if (!thumb.empty() && fs::is_regular_file(d / thumb, ec)) t.thumbnail = (d / thumb).string();
  const std::string proj = j.string_at("project", "project");
  if (!proj.empty() && fs::is_regular_file(d / proj / "project.json", ec)) {
    t.project_dir = (d / proj).string();
  }
  t.group = user ? "mine" : j.string_at("group", "samples");
  if (t.group != "empty" && t.group != "mine") t.group = "samples";
  if (j.has("alt_dimension")) {
    t.alt_kind = project_kind_from_dimension(j.string_at("alt_dimension"));
    if (t.alt_kind == ProjectKind::VR) t.alt_kind = ProjectKind::ThreeD;
    t.alt_generator = j.string_at("alt_generator");
    const std::string alt = j.string_at("alt_project");
    if (!alt.empty() && fs::is_regular_file(d / alt / "project.json", ec)) {
      t.alt_project_dir = (d / alt).string();
    }
    t.has_alt = t.alt_kind != t.kind && (!t.alt_project_dir.empty() || !t.alt_generator.empty());
  }
  if (t.name.empty()) {
    if (error) *error = dir + ": template has no name";
    return false;
  }
  *out = std::move(t);
  return true;
}

void TemplateStore::refresh(const std::string& projects_root) {
  refresh_dirs(engine_template_dirs(), user_template_dir(projects_root));
}

void TemplateStore::refresh_dirs(const std::vector<std::string>& engine_dirs,
                                 const std::string& user_dir) {
  templates_.clear();
  errors_.clear();
  auto scan = [&](const std::string& root, bool user) {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (e.is_directory(ec) && fs::exists(e.path() / "template.json", ec)) dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& p : dirs) {
      ProjectTemplate t;
      std::string err;
      if (!load_template(p.string(), user, &t, &err)) {
        errors_.push_back(err);
        continue;
      }
      if (find(t.id)) continue;  // first folder wins
      templates_.push_back(std::move(t));
    }
  };
  for (const std::string& d : engine_dirs) scan(d, false);
  scan(user_dir, true);
  std::stable_sort(templates_.begin(), templates_.end(),
                   [](const ProjectTemplate& a, const ProjectTemplate& b) {
                     if (a.user != b.user) return !a.user;
                     if (a.order != b.order) return a.order < b.order;
                     return a.name < b.name;
                   });
}

const ProjectTemplate* TemplateStore::find(const std::string& id) const {
  for (const ProjectTemplate& t : templates_) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

bool TemplateStore::create_project(ProjectStore& store, const ProjectTemplate& tpl,
                                   const std::string& raw_name, ProjectInfo* out,
                                   std::string* error, int vr_choice, int dimension) {
  const std::string name = trim(raw_name);
  // The 2D / 3D switch: build the template's alt dimension instead.
  const ProjectKind want = dimension < 0 ? tpl.kind
                           : dimension == 0 ? ProjectKind::TwoD : ProjectKind::ThreeD;
  const bool use_alt = tpl.has_alt && want == tpl.alt_kind && want != tpl.kind;
  const ProjectKind kind = use_alt ? tpl.alt_kind : tpl.kind;
  const bool vr = kind != ProjectKind::TwoD && (vr_choice < 0 ? tpl.vr : vr_choice != 0);
  if (!ProjectStore::validate_project_name(name, error)) return false;
  const std::vector<std::string>* plugins = tpl.plugins_set ? &tpl.plugins : nullptr;
  if (tpl.blank() && !use_alt) {
    if (!store.create_project(name, vr ? ProjectKind::VR : tpl.kind, plugins, out)) {
      if (error) *error = store.last_error();
      return false;
    }
    return true;
  }
  if (store.projects_root().empty()) {
    if (error) *error = "Cannot create project: projects_root is empty.";
    return false;
  }
  const fs::path dir = fs::path(store.projects_root()) / ProjectStore::make_slug(name);
  std::error_code ec;
  if (fs::exists(dir, ec)) {
    if (error) {
      *error = "A project folder already exists for \"" + name + "\" (folder '" +
               dir.filename().string() + "'). Choose a different name.";
    }
    return false;
  }
  bool ok = false;
  std::string err;
  const std::string& from = use_alt ? tpl.alt_project_dir : tpl.project_dir;
  if (!from.empty()) {
    ok = copy_tree(from, dir, &err);
  } else {
    ok = run_generator(use_alt ? tpl.alt_generator : tpl.generator, dir.string(), name, &err);
  }
  if (ok) ok = stamp_project_json(dir, name, kind, vr, plugins, &err);
  if (ok) fs::create_directories(dir / "assets", ec);
  if (!ok) {
    fs::remove_all(dir, ec);
    if (error) *error = "Template \"" + tpl.name + "\": " + err;
    return false;
  }
  if (!store.refresh()) {
    if (error) *error = store.last_error();
    return false;
  }
  if (out) {
    std::string lerr;
    if (!ProjectStore::load_project_json(dir.string(), out, &lerr)) {
      if (error) *error = lerr;
      return false;
    }
    for (const ProjectInfo& p : store.projects()) {
      if (p.id == out->id) *out = p;
    }
  }
  return true;
}

bool TemplateStore::save_as_template(const ProjectInfo& project, const std::string& user_dir,
                                     const std::string& raw_name, const std::string& description,
                                     ProjectTemplate* out, std::string* error) {
  const std::string name = trim(raw_name);
  if (!ProjectStore::validate_project_name(name, error)) return false;
  if (user_dir.empty()) {
    if (error) *error = "no template folder (projects root is empty)";
    return false;
  }
  std::error_code ec;
  if (!fs::is_regular_file(fs::path(project.path) / "project.json", ec)) {
    if (error) *error = "not a project folder: " + project.path;
    return false;
  }
  const fs::path dir = fs::path(user_dir) / ProjectStore::make_slug(name);
  if (fs::exists(dir, ec)) {
    if (error) *error = "A template named \"" + name + "\" already exists. Choose another name.";
    return false;
  }
  std::string err;
  if (!copy_tree(project.path, dir / "project", &err)) {
    fs::remove_all(dir, ec);
    if (error) *error = err;
    return false;
  }
  store::Json j = store::Json::object();
  j.set("id", dir.filename().string());
  j.set("name", name);
  j.set("dimension", dimension_string(project.kind));
  if (project.kind != ProjectKind::TwoD) j.set("vr", vr_enabled(project));
  j.set("description", description.empty()
                           ? "Saved from \"" + project.name + "\"."
                           : description);
  if (project.plugins_set) {
    store::Json arr = store::Json::array();
    for (const std::string& id : project.plugins) arr.push(id);
    j.set("plugins", arr);
  }
  store::Json tags = store::Json::array();
  tags.push("mine");
  j.set("tags", tags);
  j.set("project", "project");
  j.set("source_project", project.name);
  j.set("saved", now_utc());
  for (const char* pic : {"thumbnail.png", "icon.png"}) {
    if (fs::is_regular_file(fs::path(project.path) / pic, ec)) {
      fs::copy_file(fs::path(project.path) / pic, dir / "thumbnail.png",
                    fs::copy_options::overwrite_existing, ec);
      j.set("thumbnail", "thumbnail.png");
      break;
    }
  }
  if (!store::write_text_file((dir / "template.json").string(), j.dump(), &err)) {
    fs::remove_all(dir, ec);
    if (error) *error = err;
    return false;
  }
  if (out && !load_template(dir.string(), true, out, error)) return false;
  return true;
}

std::vector<std::string> TemplateStore::generator_ids() {
  std::vector<std::string> ids = {"sample-2d-platformer", "ghost-town-3d", "haunted-saloon-vr",
                                   survival::kSampleId, shipyard::kSampleId};
  for (const std::string& id : empty_templates::ids()) ids.push_back(id);
  return ids;
}

bool TemplateStore::run_generator(const std::string& id, const std::string& dir,
                                  const std::string& name, std::string* error) {
  if (id == "sample-2d-platformer") {
    // What the editor writes the first time a 2D claim opens: the rider
    // sheet, sample scripts and the Dusty Town starter scene.
    ProjectInfo p;
    p.id = fs::path(dir).filename().string();
    p.name = name;
    p.kind = ProjectKind::TwoD;
    p.path = dir;
    p.created = now_utc();
    p.plugins = {"ts.posse"};
    p.plugins_set = true;
    if (!ProjectStore::write_project_json(p, error)) return false;
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "assets", ec);
    sample_rider::write_into_project(dir);
    sample_scripts::write_into_project(dir);
    Workspace2D ws;
    ws.reset_defaults();
    if (!scene_io::save(ws, (fs::path(dir) / "scene.json").string())) {
      if (error) *error = "could not write scene.json";
      return false;
    }
    return true;
  }
  if (id == "ghost-town-3d") return scene3d::write_ghost_town_sample(dir, name, error);
  if (id == "haunted-saloon-vr") return scene3d::write_haunted_saloon_sample(dir, name, error);
  if (id == survival::kSampleId) return survival::write_frontier_sample(dir, name, error);
  if (id == shipyard::kSampleId) return shipyard::write_sky_pirates_sample(dir, name, error);
  if (empty_templates::handles(id)) return empty_templates::write(id, dir, name, error);
  if (error) *error = "unknown template generator '" + id + "'";
  return false;
}

int TemplateStore::bake(const std::string& templates_root, std::vector<std::string>* errors) {
  int baked = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(templates_root, ec)) {
    ProjectTemplate t;
    std::string err;
    if (!e.is_directory(ec) || !fs::exists(e.path() / "template.json", ec)) continue;
    if (!load_template(e.path().string(), false, &t, &err)) {
      if (errors) errors->push_back(err);
      continue;
    }
    if (t.generator.empty()) continue;
    store::Json tj;
    store::read_json_file((e.path() / "template.json").string(), &tj, &err);
    std::vector<std::pair<std::string, std::string>> jobs = {
        {tj.string_at("project", "project"), t.generator}};
    if (!t.alt_generator.empty() && !tj.string_at("alt_project").empty()) {
      jobs.emplace_back(tj.string_at("alt_project"), t.alt_generator);
    }
    bool all = true;
    for (const auto& [folder, generator] : jobs) {
      const fs::path out = e.path() / folder;
      fs::remove_all(out, ec);
      if (!run_generator(generator, out.string(), t.name, &err)) {
        if (errors) errors->push_back(t.id + ": " + err);
        all = false;
        break;
      }
      // Baked copies carry no timestamps; create_project stamps its own.
      store::Json j;
      if (store::read_json_file((out / "project.json").string(), &j, &err)) {
        store::Json clean = store::Json::object();
        for (const auto& m : j.members()) {
          if (m.first != "created" && m.first != "last_opened") clean.set(m.first, m.second);
        }
        store::write_text_file((out / "project.json").string(), clean.dump(), &err);
      }
    }
    if (all) ++baked;
  }
  return baked;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
