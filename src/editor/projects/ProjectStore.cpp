#include "editor/projects/ProjectStore.h"

#include "editor/settings/JsonMini.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

ProjectStore::ProjectStore(std::string projects_root)
    : projects_root_(std::move(projects_root)) {}

void ProjectStore::set_projects_root(std::string projects_root) {
  projects_root_ = std::move(projects_root);
  projects_.clear();
  last_error_.clear();
}

std::string ProjectStore::trim_copy(const std::string& s) {
  std::size_t begin = 0;
  while (begin < s.size() &&
         std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  std::size_t end = s.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

std::string ProjectStore::now_timestamp() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t t = clock::to_time_t(now);
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

std::string ProjectStore::make_slug(const std::string& name) {
  std::string slug;
  slug.reserve(name.size());
  bool pending_dash = false;
  for (unsigned char ch : name) {
    if (std::isalnum(ch)) {
      if (pending_dash && !slug.empty()) {
        slug.push_back('-');
      }
      pending_dash = false;
      slug.push_back(static_cast<char>(std::tolower(ch)));
    } else if (!slug.empty()) {
      pending_dash = true;
    }
  }
  if (slug.empty()) {
    slug = "project";
  }
  return slug;
}

bool ProjectStore::validate_project_name(const std::string& name,
                                         std::string* error_out) {
  const std::string trimmed = trim_copy(name);
  if (trimmed.empty()) {
    if (error_out) {
      *error_out = "Project name must be non-empty";
    }
    return false;
  }
  if (trimmed.size() > 64) {
    if (error_out) {
      *error_out = "Project name must be 64 characters or fewer";
    }
    return false;
  }

  for (unsigned char ch : trimmed) {
    if (std::isalnum(ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '.') {
      continue;
    }
    if (error_out) {
      *error_out =
          "Project name may only contain letters, digits, spaces, '-', '_', "
          "and '.' (no path separators or special characters)";
    }
    return false;
  }

  // Reject names that are only punctuation / would collide with reserved.
  const std::string slug = make_slug(trimmed);
  if (slug == "project" && !std::any_of(trimmed.begin(), trimmed.end(),
                                        [](unsigned char c) {
                                          return static_cast<bool>(
                                              std::isalnum(c));
                                        })) {
    if (error_out) {
      *error_out = "Project name must include at least one letter or digit";
    }
    return false;
  }

  if (trimmed == "." || trimmed == "..") {
    if (error_out) {
      *error_out = "Project name cannot be '.' or '..'";
    }
    return false;
  }

  return true;
}

bool ProjectStore::load_project_json(const std::string& project_dir,
                                     ProjectInfo* out,
                                     std::string* error_out) {
  if (!out) {
    if (error_out) {
      *error_out = "load_project_json: out is null";
    }
    return false;
  }

  const fs::path dir = project_dir;
  const fs::path json_path = dir / "project.json";
  if (!fs::exists(json_path)) {
    if (error_out) {
      *error_out = "Missing project.json in " + dir.string();
    }
    return false;
  }

  std::ifstream in(json_path);
  if (!in) {
    if (error_out) {
      *error_out = "Unreadable project.json (could not open): " +
                   json_path.string();
    }
    return false;
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  auto parsed = json_mini::parse_object(ss.str());
  if (!parsed) {
    if (error_out) {
      *error_out = "Unreadable project.json (invalid JSON): " +
                   json_path.string();
    }
    return false;
  }

  ProjectInfo info;
  info.path = dir.string();
  info.id = dir.filename().string();
  info.name = json_mini::get_string(*parsed, "name", info.id);
  const std::string dimension =
      json_mini::get_string(*parsed, "dimension", "2d");
  info.kind = project_kind_from_dimension(dimension);
  info.created = json_mini::get_string(*parsed, "created");
  info.last_opened = json_mini::get_string(*parsed, "last_opened");
  // "plugins" arrived with the plugin system; absent = defaults.
  if (parsed->count("plugins") != 0) {
    info.plugins_set = true;
    info.plugins = split_plugin_ids(json_mini::get_string(*parsed, "plugins"));
  }
  *out = std::move(info);
  return true;
}

std::vector<std::string> ProjectStore::split_plugin_ids(
    const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    std::size_t a = 0;
    std::size_t b = cur.size();
    while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
    while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) --b;
    std::string id = cur.substr(a, b - a);
    if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) {
      out.push_back(std::move(id));
    }
    cur.clear();
  };
  for (char c : text) {
    if (c == ',') {
      flush();
    } else {
      cur += c;
    }
  }
  flush();
  return out;
}

std::string ProjectStore::join_plugin_ids(const std::vector<std::string>& ids) {
  std::string out;
  for (const std::string& id : ids) {
    if (id.empty()) continue;
    if (!out.empty()) out += ",";
    out += id;
  }
  return out;
}

bool ProjectStore::write_project_json(const ProjectInfo& project,
                                      std::string* error_out) {
  if (project.path.empty()) {
    if (error_out) {
      *error_out = "Project path is empty";
    }
    return false;
  }

  std::error_code ec;
  fs::create_directories(project.path, ec);
  if (ec) {
    if (error_out) {
      *error_out = "Could not create project folder '" + project.path +
                   "': " + ec.message();
    }
    return false;
  }

  const fs::path json_path = fs::path(project.path) / "project.json";
  std::ofstream out(json_path, std::ios::trunc);
  if (!out) {
    if (error_out) {
      *error_out = "Could not write " + json_path.string();
    }
    return false;
  }

  std::vector<std::pair<std::string, std::string>> fields = {
      {"name", project.name},
      {"dimension", dimension_string(project.kind)},
      {"created", project.created},
  };
  if (!project.last_opened.empty()) {
    fields.emplace_back("last_opened", project.last_opened);
  }
  if (project.plugins_set) {
    fields.emplace_back("plugins", join_plugin_ids(project.plugins));
  }

  out << json_mini::write_object(fields);
  if (!out) {
    if (error_out) {
      *error_out = "Failed while writing " + json_path.string();
    }
    return false;
  }
  return true;
}

bool ProjectStore::seed_sample_2d_if_empty() {
  if (!projects_.empty()) {
    return true;
  }

  ProjectInfo sample;
  sample.id = "sample-2d-platformer";
  sample.name = "Sample 2D Platformer";
  sample.kind = ProjectKind::TwoD;
  sample.path = (fs::path(projects_root_) / sample.id).string();
  sample.created = now_timestamp();

  if (!write_project_json(sample, &last_error_)) {
    return false;
  }
  projects_.push_back(std::move(sample));
  return true;
}

bool ProjectStore::refresh() {
  projects_.clear();
  last_error_.clear();

  if (projects_root_.empty()) {
    last_error_ =
        "projects_root is empty -- set a folder in Settings "
        "(default: ./TombStoneProjects)";
    return false;
  }

  const fs::path root = projects_root_;
  std::error_code ec;
  if (!fs::exists(root)) {
    fs::create_directories(root, ec);
    if (ec) {
      last_error_ = "Missing projects_root '" + root.string() +
                    "' and could not create it: " + ec.message();
      return false;
    }
  } else if (!fs::is_directory(root)) {
    last_error_ = "projects_root is not a directory: " + root.string();
    return false;
  }

  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (ec) {
      last_error_ = "Failed scanning projects_root '" + root.string() +
                    "': " + ec.message();
      return false;
    }
    if (!entry.is_directory()) {
      continue;
    }
    ProjectInfo info;
    std::string err;
    if (!load_project_json(entry.path().string(), &info, &err)) {
      // Skip folders without a valid project.json (missing / unreadable).
      continue;
    }
    projects_.push_back(std::move(info));
  }

  std::sort(projects_.begin(), projects_.end(),
            [](const ProjectInfo& a, const ProjectInfo& b) {
              return a.name < b.name;
            });

  if (!seed_sample_2d_if_empty()) {
    return false;
  }
  return true;
}

bool ProjectStore::create_project_2d(const std::string& name,
                                     ProjectInfo* out) {
  return create_project(name, ProjectKind::TwoD, nullptr, out);
}

bool ProjectStore::create_project(const std::string& name, ProjectKind kind,
                                  const std::vector<std::string>* plugins,
                                  ProjectInfo* out) {
  last_error_.clear();

  std::string trimmed = trim_copy(name);
  if (!validate_project_name(trimmed, &last_error_)) {
    return false;
  }

  if (projects_root_.empty()) {
    last_error_ =
        "Cannot create project: projects_root is empty. Open Settings and "
        "set a projects folder.";
    return false;
  }

  std::error_code ec;
  if (!fs::exists(projects_root_)) {
    fs::create_directories(projects_root_, ec);
    if (ec) {
      last_error_ = "Missing projects_root '" + projects_root_ +
                    "' and could not create it: " + ec.message();
      return false;
    }
  }

  const std::string slug = make_slug(trimmed);
  const fs::path dir = fs::path(projects_root_) / slug;
  if (fs::exists(dir)) {
    last_error_ = "A project folder already exists for \"" + trimmed +
                  "\" (folder '" + dir.filename().string() +
                  "'). Choose a different name.";
    return false;
  }

  ProjectInfo info;
  info.id = dir.filename().string();
  info.name = trimmed;
  info.kind = kind;
  info.path = dir.string();
  info.created = now_timestamp();
  if (plugins) {
    info.plugins = *plugins;
    info.plugins_set = true;
  }

  if (!write_project_json(info, &last_error_)) {
    return false;
  }
  // Sprite / tileset images live in <project>/assets (Supply Wagon panel).
  // Best effort: the editor recreates it on open if this fails.
  fs::create_directories(dir / "assets", ec);

  if (!refresh()) {
    return false;
  }

  if (out) {
    for (const auto& p : projects_) {
      if (p.path == info.path || p.id == info.id) {
        *out = p;
        return true;
      }
    }
    *out = info;
  }
  return true;
}

bool ProjectStore::touch_last_opened(const std::string& project_path) {
  last_error_.clear();
  ProjectInfo info;
  if (!load_project_json(project_path, &info, &last_error_)) {
    return false;
  }
  info.last_opened = now_timestamp();
  if (!write_project_json(info, &last_error_)) {
    return false;
  }
  for (auto& p : projects_) {
    if (p.path == info.path || p.id == info.id) {
      p.last_opened = info.last_opened;
      break;
    }
  }
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
