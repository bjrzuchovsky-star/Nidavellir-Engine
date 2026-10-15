#include "SmokeTemplates.h"

#include "editor/projects/ProjectStore.h"
#include "editor/templates/TemplateStore.h"
#include "store/Json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;

namespace {

int fail(const std::string& why) {
  std::cerr << "[smoke] templates: " << why << '\n';
  return 1;
}

store::Json read_json(const fs::path& p) {
  store::Json j;
  store::read_json_file(p.string(), &j, nullptr);
  return j;
}

}  // namespace

int run_bake_templates(const char* dir) {
  std::vector<std::string> errors;
  const int n = editor::TemplateStore::bake(dir, &errors);
  for (const std::string& e : errors) std::cerr << "[bake] " << e << '\n';
  std::cout << "[bake] " << n << " template project(s) written under " << dir << '\n';
  return errors.empty() && n > 0 ? 0 : 1;
}

int run_templates_smoke() {
  const fs::path root = fs::path("TombStoneProjects") / "_templates_smoke";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  // 1. The engine templates.
  editor::TemplateStore ts;
  ts.refresh_dirs(editor::TemplateStore::engine_template_dirs(),
                  editor::TemplateStore::user_template_dir(root.string()));
  for (const std::string& e : ts.errors()) std::cerr << "[smoke] template error: " << e << '\n';
  if (!ts.errors().empty()) return fail("engine templates should all load");
  const char* want[] = {"blank-2d", "blank-3d", "dusty-town", "frontier-survival",
                        "sky-pirates", "ghost-town-3d", "haunted-saloon-vr"};
  for (const char* id : want) {
    if (!ts.find(id)) return fail(std::string("missing engine template ") + id);
  }
  for (const auto& t : ts.templates()) {
    if (t.kind == editor::ProjectKind::VR) return fail("no template is a VR dimension: " + t.id);
    if (t.description.empty()) return fail("template without a description: " + t.id);
    if (!t.blank() && t.project_dir.empty()) return fail("sample template not baked: " + t.id);
  }
  const auto* saloon = ts.find("haunted-saloon-vr");
  if (saloon->kind != editor::ProjectKind::ThreeD || !saloon->vr) {
    return fail("haunted-saloon-vr is 3D with VR on");
  }
  if (ts.find("blank-2d")->plugins != std::vector<std::string>{"ts.posse"} ||
      ts.find("frontier-survival")->plugins != std::vector<std::string>{"ts.frontier"}) {
    return fail("template plugin lists");
  }

  // 2. A fresh root stays empty: samples are templates, not seeds.
  editor::ProjectStore store(root.string());
  if (!store.refresh() || !store.projects().empty()) return fail("a fresh root is not seeded");
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (e.path().filename() != ".templates") return fail("seeded " + e.path().string());
  }

  // 3. Create from every template.
  int made = 0;
  std::string dusty_name;
  for (const auto& t : ts.templates()) {
    const std::string name = "From " + t.name.substr(0, t.name.find(' ')) + " " + std::to_string(made);
    editor::ProjectInfo p;
    std::string err;
    if (!editor::TemplateStore::create_project(store, t, name, &p, &err)) {
      return fail("create from " + t.id + ": " + err);
    }
    ++made;
    if (t.id == "dusty-town") dusty_name = name;
    if (p.name != name || p.kind != t.kind || p.vr != t.vr ||
        fs::path(p.path).filename() != editor::ProjectStore::make_slug(name)) {
      return fail("created project fields for " + t.id);
    }
    if (t.plugins_set && (!p.plugins_set || p.plugins != t.plugins)) {
      return fail("created project plugins for " + t.id);
    }
    const store::Json pj = read_json(fs::path(p.path) / "project.json");
    if (pj.string_at("dimension") == "vr" || pj.has("last_opened") || pj.string_at("created").empty()) {
      return fail("project.json stamp for " + t.id + ": " + pj.dump());
    }
    if (!t.blank() && !fs::exists(fs::path(p.path) / "scene.json")) {
      return fail("template content copied for " + t.id);
    }
    if (!fs::is_directory(fs::path(p.path) / "assets")) return fail("assets folder for " + t.id);
  }
  if (store.projects().size() != static_cast<std::size_t>(made)) return fail("store lists every claim");
  std::string err;
  if (editor::TemplateStore::create_project(store, *ts.find("dusty-town"), dusty_name, nullptr, &err)) {
    return fail("a taken name must fail");
  }
  if (editor::TemplateStore::create_project(store, *ts.find("dusty-town"), "bad/name", nullptr, &err)) {
    return fail("an unsafe name must fail");
  }

  // 4. The VR switch in the create flow (3D only).
  editor::ProjectInfo vr3d, flat_saloon, flat2d;
  if (!editor::TemplateStore::create_project(store, *ts.find("blank-3d"), "Goggles On", &vr3d, &err, 1) ||
      !vr3d.vr || vr3d.kind != editor::ProjectKind::ThreeD || !editor::vr_enabled(vr3d)) {
    return fail("blank 3D with VR on: " + err);
  }
  if (!editor::TemplateStore::create_project(store, *saloon, "Saloon Flat", &flat_saloon, &err, 0) ||
      flat_saloon.vr) {
    return fail("saloon with VR off: " + err);
  }
  if (!editor::TemplateStore::create_project(store, *ts.find("dusty-town"), "Dusty VR", &flat2d, &err, 1) ||
      flat2d.vr) {
    return fail("2D ignores the VR switch");
  }

  // 5. An old root (samples + marker files from the seeding days) is left alone.
  const fs::path old_root = root / "_old_root";
  fs::create_directories(old_root / "sample-2d-platformer");
  std::ofstream(old_root / "sample-2d-platformer" / "project.json")
      << "{\"name\": \"Sample 2D Platformer\", \"dimension\": \"2d\", \"created\": \"x\"}\n";
  std::ofstream(old_root / ".haunted-saloon-vr.seeded") << "seeded\n";
  editor::ProjectStore old_store(old_root.string());
  if (!old_store.refresh() || old_store.projects().size() != 1 ||
      fs::exists(old_root / "haunted-saloon-vr") || fs::exists(old_root / "sky-pirates")) {
    return fail("old roots keep what they have and gain nothing");
  }
  fs::remove_all(old_root, ec);

  // 6. Save as Template, then break ground with it.
  editor::ProjectInfo src;
  if (!editor::TemplateStore::create_project(store, *ts.find("frontier-survival"), "My Homestead", &src, &err)) {
    return fail(err);
  }
  std::ofstream(fs::path(src.path) / "scripts_note.txt") << "custom\n";
  std::ofstream(fs::path(src.path) / "imgui.ini") << "[Window]\n";
  fs::create_directories(fs::path(src.path) / ".cache");
  editor::ProjectTemplate mine;
  const std::string user_dir = editor::TemplateStore::user_template_dir(root.string());
  if (!editor::TemplateStore::save_as_template(src, user_dir, "Homestead Kit", "My survival start",
                                               &mine, &err)) {
    return fail("save as template: " + err);
  }
  if (editor::TemplateStore::save_as_template(src, user_dir, "Homestead Kit", "", nullptr, &err)) {
    return fail("a taken template name must fail");
  }
  if (fs::exists(fs::path(mine.dir) / "project" / "imgui.ini") ||
      fs::exists(fs::path(mine.dir) / "project" / ".cache")) {
    return fail("templates skip imgui.ini and dot folders");
  }
  ts.refresh_dirs(editor::TemplateStore::engine_template_dirs(), user_dir);
  const auto* kit = ts.find("homestead-kit");
  if (!kit || !kit->user || kit->description != "My survival start" ||
      kit->plugins != std::vector<std::string>{"ts.frontier"} || ts.templates().back().id != "homestead-kit") {
    return fail("the saved template is listed as mine, last, with its plugins");
  }
  if (!store.refresh()) return fail("refresh");
  for (const auto& p : store.projects()) {
    if (p.path.find(".templates") != std::string::npos) return fail("user templates are not claims");
  }
  editor::ProjectInfo again;
  if (!editor::TemplateStore::create_project(store, *kit, "Second Homestead", &again, &err) ||
      !fs::exists(fs::path(again.path) / "scripts_note.txt") || again.name != "Second Homestead" ||
      again.plugins != std::vector<std::string>{"ts.frontier"}) {
    return fail("create from my template: " + err);
  }

  // 7. A template without a baked project/ falls back to its generator.
  const fs::path loose = root / "_loose_templates" / "ghost-town-3d";
  fs::create_directories(loose);
  fs::copy_file(fs::path(ts.find("ghost-town-3d")->dir) / "template.json", loose / "template.json");
  editor::TemplateStore ts2;
  ts2.refresh_dirs({(root / "_loose_templates").string()}, {});
  const auto* g = ts2.find("ghost-town-3d");
  editor::ProjectInfo gen;
  if (!g || !g->project_dir.empty() ||
      !editor::TemplateStore::create_project(store, *g, "Generated Town", &gen, &err) ||
      !fs::exists(fs::path(gen.path) / "scene.json") || gen.name != "Generated Town") {
    return fail("generator fallback: " + err);
  }

  fs::remove_all(root, ec);
  std::cout << "[smoke] templates OK (" << made
            << " engine templates created, VR switch, no seeding, Save as Template, generator fallback)\n";
  return 0;
}
