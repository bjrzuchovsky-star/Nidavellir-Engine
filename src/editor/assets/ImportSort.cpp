#include "editor/assets/ImportSort.h"

#include "render3d/ModelImport.h"
#include "scene/JsonTree.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {
namespace importsort {

namespace fs = std::filesystem;
using json::Value;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && lower(s.substr(s.size() - tail.size())) == tail;
}

std::string ext_of(const std::string& name) { return lower(fs::u8path(name).extension().string()); }

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool spit(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

std::string percent_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string stem_of(const std::string& name) {
  std::string n = fs::u8path(name).filename().string();
  if (ends_with(n, ".prefab.json")) return n.substr(0, n.size() - 12);
  const auto dot = n.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

fs::path norm(const fs::path& p) {
  std::error_code ec;
  fs::path a = fs::absolute(p, ec);
  return (ec ? p : a).lexically_normal();
}

bool is_under(const fs::path& file, const fs::path& dir) {
  const fs::path r = file.lexically_relative(dir);
  const std::string s = r.generic_string();
  return !s.empty() && s.rfind("..", 0) != 0 && !r.is_absolute();
}

// A free name in `dir`: name.ext, name_2.ext...
fs::path unique_file(const fs::path& want, const std::set<std::string>& taken) {
  std::error_code ec;
  auto free = [&](const fs::path& p) { return !fs::exists(p, ec) && !taken.count(p.generic_string()); };
  if (free(want)) return want;
  const std::string fname = want.filename().string();
  std::string stem = fname, ext;
  const auto dot = fname.find('.');
  if (dot != std::string::npos && dot > 0) {
    stem = fname.substr(0, dot);
    ext = fname.substr(dot);  // keeps ".prefab.json" / ".anim.json" whole
  }
  for (int n = 2; n < 10000; ++n) {
    const fs::path p = want.parent_path() / fs::u8path(stem + "_" + std::to_string(n) + ext);
    if (free(p)) return p;
  }
  return want;
}

fs::path unique_dir(const fs::path& want) {
  std::error_code ec;
  if (!fs::exists(want, ec)) return want;
  for (int n = 2; n < 10000; ++n) {
    const fs::path p = want.parent_path() / fs::u8path(want.filename().string() + "_" + std::to_string(n));
    if (!fs::exists(p, ec)) return p;
  }
  return want;
}

std::mutex g_drop_mu;
std::vector<std::vector<std::string>> g_drops;

}  // namespace

Settings load_settings(const std::string& claim_dir) {
  Settings s;
  Value v;
  std::string err;
  if (json::parse(slurp(fs::u8path(claim_dir) / fs::u8path(kSettingsFile)), &v, &err) && v.is_object()) {
    s.sort_imports = v.flag("sort_imports", true);
    s.keep_model_packs = v.flag("keep_model_packs", true);
  }
  return s;
}

bool save_settings(const std::string& claim_dir, const Settings& s, std::string* error) {
  const fs::path p = fs::u8path(claim_dir) / fs::u8path(kSettingsFile);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  Value v = Value::object();
  v.set("sort_imports", Value::boolean(s.sort_imports));
  v.set("keep_model_packs", Value::boolean(s.keep_model_packs));
  if (!spit(p, json::write(v))) {
    if (error) *error = "cannot write " + p.string();
    return false;
  }
  return true;
}

bool is_texture_file(const std::string& path) {
  static const char* k[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".dds",
                            ".ktx", ".ktx2", ".hdr", ".exr", ".vtf", ".webp", ".tif", ".tiff"};
  const std::string e = ext_of(path);
  for (const char* x : k) {
    if (e == x) return true;
  }
  return false;
}

const std::vector<std::string>& type_folders() {
  static const std::vector<std::string> v = {"assets/Models", "assets/Textures", "assets/Materials",
                                             "assets/Audio",  "assets/Fonts",    "prefabs",
                                             "assets/Scenes", "scripts",         "assets/Animations",
                                             "assets/Other"};
  return v;
}

std::string type_folder(const std::string& file_name) {
  const std::string n = lower(fs::u8path(file_name).filename().string());
  const std::string e = ext_of(n);
  if (render3d::is_model_file(n)) return "assets/Models";
  if (is_texture_file(n)) return "assets/Textures";
  if (e == ".mtl" || e == ".vmt" || e == ".mat" || ends_with(n, ".material.json")) return "assets/Materials";
  if (e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac") return "assets/Audio";
  if (e == ".ttf" || e == ".otf" || e == ".ttc" || e == ".fnt") return "assets/Fonts";
  if (ends_with(n, ".prefab.json")) return "prefabs";
  if (n == "scene.json" || ends_with(n, ".scene.json") || ends_with(n, ".scene3d.json") || e == ".tsscene") {
    return "assets/Scenes";
  }
  if (e == ".lua" || e == ".cs" || e == ".cpp" || e == ".h" || e == ".hpp" || ends_with(n, ".graph.json")) {
    return "scripts";
  }
  if (ends_with(n, ".anim.json") || e == ".anim") return "assets/Animations";
  return "assets/Other";
}

std::vector<std::string> model_textures(const std::string& model_abs) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::error_code ec;
  const fs::path model = norm(fs::u8path(model_abs));
  const fs::path dir = model.parent_path();
  auto add = [&](const fs::path& p) {
    const fs::path n = norm(p);
    if (fs::is_regular_file(n, ec) && seen.insert(n.string()).second) out.push_back(n.string());
  };
  const std::string e = ext_of(model_abs);
  if (e == ".gltf") {
    Value g;
    std::string err;
    if (json::parse(slurp(model), &g, &err)) {
      for (const Value& img : g["images"].items()) {
        const std::string uri = img.str("uri");
        if (!uri.empty() && uri.rfind("data:", 0) != 0) add(dir / fs::u8path(percent_decode(uri)));
      }
    }
    return out;
  }
  if (e == ".obj") {
    for (const std::string& dep : render3d::model_dependencies(model.string())) {
      if (!ends_with(dep, ".mtl")) continue;
      std::istringstream in(slurp(fs::u8path(dep)));
      for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        const std::string k = lower(key);
        if (k.rfind("map_", 0) != 0 && k != "bump" && k != "disp" && k != "decal" && k != "norm" && k != "refl") continue;
        std::string tok, last;
        while (ls >> tok) last = tok;
        if (!last.empty()) add(fs::u8path(dep).parent_path() / fs::u8path(last));
      }
    }
    return out;
  }
  if (e == ".dae") {
    const std::string text = slurp(model);
    for (std::size_t pos = text.find("<init_from>"); pos != std::string::npos; pos = text.find("<init_from>", pos + 1)) {
      const std::size_t a = pos + 11, b = text.find("</init_from>", a);
      if (b == std::string::npos) break;
      std::string ref = percent_decode(text.substr(a, b - a));
      if (ref.rfind("file://", 0) == 0) ref = ref.substr(7);
      if (is_texture_file(ref)) add(dir / fs::u8path(ref));
    }
    if (!out.empty()) return out;
  }
  // Model pack heuristic (FBX, GLB, 3DS, .blend...): images beside it.
  int models_here = 0;
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    if (f.is_regular_file(ec) && render3d::is_model_file(f.path().string())) ++models_here;
  }
  const std::string stem = lower(stem_of(model.string()));
  auto consider = [&](const fs::path& p) {
    if (!is_texture_file(p.string())) return;
    if (models_here <= 1 || lower(p.filename().string()).rfind(stem, 0) == 0) add(p);
  };
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    if (f.is_regular_file(ec)) consider(f.path());
    if (!f.is_directory(ec)) continue;
    const std::string sub = lower(f.path().filename().string());
    if (sub == "textures" || sub == "texture" || sub == "materials" || sub == "tex" || sub == "maps" || sub == "images") {
      for (fs::recursive_directory_iterator it(f.path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 2) it.disable_recursion_pending();
        if (it->is_regular_file(ec)) consider(it->path());
      }
    }
  }
  return out;
}

bool import_files(const std::string& claim_dir, const std::vector<std::string>& sources,
                  const std::string& dest_folder_in, const Settings& settings, Result* out) {
  *out = Result{};
  std::error_code ec;
  const fs::path root = norm(fs::u8path(claim_dir));
  std::string dest_folder = dest_folder_in;
  std::replace(dest_folder.begin(), dest_folder.end(), '\\', '/');
  while (!dest_folder.empty() && dest_folder.back() == '/') dest_folder.pop_back();
  if (dest_folder.find("..") != std::string::npos) {
    out->errors.push_back("bad destination folder " + dest_folder);
    return false;
  }
  const bool sorted = dest_folder.empty() && settings.sort_imports;
  const std::string base = dest_folder.empty() ? "assets" : dest_folder;

  // Every file to bring in, with the folder it was dropped from (a dropped
  // folder keeps its structure when the destination is explicit).
  struct Src {
    fs::path file;
    fs::path drop_root;  // the dropped folder, or the file's own folder
  };
  std::vector<Src> files;
  for (const std::string& s : sources) {
    const fs::path p = norm(fs::u8path(s));
    if (is_under(p, root)) {
      out->notes.push_back(p.filename().string() + " is already in this claim (use the Asset Browser to move it)");
      continue;
    }
    if (fs::is_directory(p, ec)) {
      for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
          if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(ec) || ends_with(name, ".meta") || (!name.empty() && name[0] == '.')) continue;
        files.push_back({norm(it->path()), p.parent_path()});
      }
    } else if (fs::is_regular_file(p, ec)) {
      files.push_back({p, p.parent_path()});
    } else {
      out->errors.push_back("not found: " + s);
    }
  }

  // Models claim their sibling files and textures.
  std::map<std::string, std::vector<fs::path>> model_deps, model_tex;
  std::set<std::string> claimed;
  for (const Src& s : files) {
    const std::string f = s.file.string();
    if (!render3d::is_model_file(f)) continue;
    for (const std::string& d : render3d::model_dependencies(f)) model_deps[f].push_back(norm(fs::u8path(d)));
    for (const std::string& t : model_textures(f)) model_tex[f].push_back(fs::u8path(t));
  }
  for (const auto& [m, v] : model_deps) for (const fs::path& d : v) claimed.insert(d.string());
  for (const auto& [m, v] : model_tex) for (const fs::path& d : v) claimed.insert(d.string());

  std::set<std::string> taken;  // destinations planned this call
  std::vector<std::pair<fs::path, fs::path>> copies;  // src -> dest (absolute)
  struct Rewrite {
    fs::path file;                                     // copied model / .mtl to edit
    std::vector<std::pair<std::string, std::string>> refs;  // old ref -> new ref
  };
  std::vector<Rewrite> rewrites;
  auto rel_of = [&](const fs::path& abs) { return abs.lexically_relative(root).generic_string(); };

  for (const Src& s : files) {
    const std::string f = s.file.string();
    if (render3d::is_model_file(f)) {
      // assets/Models/<Name>/ (or the explicit folder): the model, then its
      // files at the same place relative to it.
      const fs::path mdir = s.file.parent_path();
      fs::path target_dir;
      if (sorted) {
        target_dir = unique_dir(root / "assets" / "Models" / fs::u8path(stem_of(f)));
      } else if (!dest_folder.empty()) {
        target_dir = root / fs::u8path(base) / s.file.parent_path().lexically_relative(s.drop_root);
      } else {
        target_dir = unique_dir(root / "assets" / fs::u8path(stem_of(f)));
      }
      const fs::path model_dest = unique_file(target_dir / s.file.filename(), taken);
      taken.insert(model_dest.generic_string());
      copies.emplace_back(s.file, model_dest);
      out->mains.push_back(rel_of(model_dest));
      const fs::path tex_dir = (sorted && !settings.keep_model_packs)
                                   ? unique_dir(root / "assets" / "Textures" / fs::u8path(stem_of(f)))
                                   : fs::path();
      const bool can_rewrite = ext_of(f) == ".gltf" || ext_of(f) == ".obj";
      if (!tex_dir.empty() && !can_rewrite && !model_tex[f].empty()) {
        out->notes.push_back(s.file.filename().string() + ": textures kept with the model (its format's texture paths can't be rewritten)");
      }
      Rewrite gltf_rw{model_dest, {}};
      std::map<std::string, Rewrite> mtl_rw;  // source .mtl -> edits
      auto place = [&](const fs::path& dep, bool texture) {
        fs::path dest;
        const bool inside = is_under(dep, mdir);
        if (texture && !tex_dir.empty() && can_rewrite) {
          dest = tex_dir / dep.filename();
        } else {
          dest = model_dest.parent_path() / (inside ? dep.lexically_relative(mdir) : dep.filename());
        }
        if (std::any_of(copies.begin(), copies.end(), [&](const auto& c) { return c.first == dep; })) return;
        dest = unique_file(dest, taken);
        taken.insert(dest.generic_string());
        copies.emplace_back(dep, dest);
        // Reference changes: the file is no longer where the model said.
        const std::string old_ref = dep.lexically_relative(mdir).generic_string();
        const std::string new_ref = dest.lexically_relative(model_dest.parent_path()).generic_string();
        if (texture && old_ref != new_ref) {
          if (ext_of(f) == ".gltf") {
            gltf_rw.refs.emplace_back(old_ref, new_ref);
          } else if (ext_of(f) == ".obj") {
            // OBJ: texture names live in the .mtl, relative to it.
            for (const fs::path& d : model_deps[f]) {
              if (!ends_with(d.string(), ".mtl")) continue;
              Rewrite& r = mtl_rw[d.string()];
              r.refs.emplace_back(dep.lexically_relative(d.parent_path()).generic_string(), new_ref);
            }
          }
        }
      };
      for (const fs::path& d : model_deps[f]) place(d, false);
      for (const fs::path& t : model_tex[f]) place(t, true);
      if (!gltf_rw.refs.empty()) rewrites.push_back(gltf_rw);
      for (auto& [mtl, r] : mtl_rw) {
        for (const auto& c : copies) {
          if (c.first.string() == mtl) r.file = c.second;
        }
        // The .mtl sits beside the model: its new refs are relative to it.
        if (!r.file.empty()) {
          for (auto& [o, n] : r.refs) {
            n = (model_dest.parent_path() / fs::u8path(n)).lexically_normal().lexically_relative(r.file.parent_path()).generic_string();
          }
          rewrites.push_back(r);
        }
      }
      continue;
    }
    if (claimed.count(f)) continue;  // travels with its model
    fs::path dest;
    if (sorted) {
      dest = root / fs::u8path(type_folder(f)) / s.file.filename();
    } else {
      dest = root / fs::u8path(base) / s.file.lexically_relative(s.drop_root);
    }
    dest = unique_file(dest, taken);
    taken.insert(dest.generic_string());
    copies.emplace_back(s.file, dest);
    out->mains.push_back(rel_of(dest));
  }

  for (const auto& [from, to] : copies) {
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) {
      out->errors.push_back("copy " + from.string() + " failed: " + ec.message());
      continue;
    }
    out->files.push_back(rel_of(to));
  }
  for (const Rewrite& r : rewrites) {
    std::string text = slurp(r.file);
    std::size_t n = 0;
    for (const auto& [o, nw] : r.refs) {
      if (ext_of(r.file.string()) == ".gltf") {
        const std::string a = "\"" + o + "\"", b = "\"" + nw + "\"";
        for (std::size_t pos = text.find(a); pos != std::string::npos; pos = text.find(a, pos + b.size())) {
          text.replace(pos, a.size(), b);
          ++n;
        }
      } else {
        // .mtl: the map statement's last token.
        std::istringstream in(text);
        std::string edited, line;
        while (std::getline(in, line)) {
          const bool cr = !line.empty() && line.back() == '\r';
          if (cr) line.pop_back();
          if (line.size() > o.size() && line.compare(line.size() - o.size(), o.size(), o) == 0 &&
              std::isspace(static_cast<unsigned char>(line[line.size() - o.size() - 1]))) {
            line = line.substr(0, line.size() - o.size()) + nw;
            ++n;
          }
          edited += line + (cr ? "\r\n" : "\n");
        }
        text = edited;
      }
    }
    if (n > 0 && spit(r.file, text)) {
      out->notes.push_back(rel_of(r.file) + ": " + std::to_string(n) + " texture path(s) updated");
    }
  }
  return !out->files.empty();
}

void push_os_drop(const std::vector<std::string>& paths) {
  if (paths.empty()) return;
  std::lock_guard<std::mutex> lock(g_drop_mu);
  g_drops.push_back(paths);
}

bool os_drop_pending() {
  std::lock_guard<std::mutex> lock(g_drop_mu);
  return !g_drops.empty();
}

std::vector<std::string> take_os_drop() {
  std::lock_guard<std::mutex> lock(g_drop_mu);
  if (g_drops.empty()) return {};
  std::vector<std::string> v = std::move(g_drops.front());
  g_drops.erase(g_drops.begin());
  return v;
}

}  // namespace importsort
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
