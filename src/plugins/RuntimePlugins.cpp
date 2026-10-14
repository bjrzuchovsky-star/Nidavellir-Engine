#include "plugins/RuntimePlugins.h"

#include "core/JsonMini.h"
#include "plugins/Builtins.h"
#include "runtime/World.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace plugins {

namespace {
// Editor-side built-ins a runtime host never has.
bool editor_only(const std::string& id) { return id == "ts.posse"; }
}  // namespace

ProjectPluginList read_project_plugins(const std::string& project_dir) {
  ProjectPluginList out;
  std::ifstream in(std::filesystem::path(project_dir) / "project.json");
  if (!in) return out;
  std::ostringstream ss;
  ss << in.rdbuf();
  const auto parsed = json_mini::parse_object(ss.str());
  if (!parsed) return out;
  const std::string dim = json_mini::get_string(*parsed, "dimension", "2d");
  if (dim == "3d" || dim == "3D") out.type = plugin::ProjectType::ThreeD;
  if (dim == "vr" || dim == "VR") out.type = plugin::ProjectType::VR;
  if (parsed->count("plugins") != 0) {
    out.listed = true;
    out.ids = plugin::split_plugin_list(json_mini::get_string(*parsed, "plugins"));
  }
  return out;
}

RuntimePlugins::RuntimePlugins()
    : catalog_(std::make_unique<plugin::Catalog>()),
      manager_(std::make_unique<plugin::PluginManager>(*catalog_)) {}

RuntimePlugins::~RuntimePlugins() {
  manager_.reset();
  catalog_.reset();
}

std::size_t RuntimePlugins::open(const std::string& project_dir,
                                 const std::string& host, Sink sink) {
  manager_.reset();
  catalog_ = std::make_unique<plugin::Catalog>();
  manager_ = std::make_unique<plugin::PluginManager>(*catalog_);
  manager_->set_log_sink(sink);
  std::vector<std::string> errors;
  fill_catalog(*catalog_, project_dir, &errors);
  for (const std::string& e : errors) {
    if (sink) sink({runtime::LogLevel::Error, "", "plugin wagon: " + e});
  }
  const ProjectPluginList list = read_project_plugins(project_dir);
  std::vector<std::string> ids =
      list.listed ? list.ids : plugin::PluginManager::defaults(*catalog_, list.type);
  std::vector<std::string> keep;
  for (const std::string& id : ids) {
    if (editor_only(id)) {
      if (sink) sink({runtime::LogLevel::Info, id, id + " is editor-only; not riding in " + host});
      continue;
    }
    keep.push_back(id);
  }
  plugin::PluginContext ctx;
  ctx.project_type = list.type;
  ctx.project_dir = project_dir;
  ctx.host = host;
  return manager_->load(keep, ctx);
}

void RuntimePlugins::attach(runtime::World& world) const {
  world.set_extensions(manager_->world_extensions());
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
