// Source engine material lookup (see SourceMaterials.h). Clean-room code
// from the public VMT / VTF descriptions.
#include "render3d/import/SourceMaterials.h"

#include "render3d/ModelImport.h"
#include "render3d/import/SourceFormats.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ts {
namespace tombstone {
namespace render3d {
namespace source {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

bool read_text(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Exact path, else a case-insensitive walk (Source content is mixed case).
bool find_ci(const fs::path& root, const std::string& rel, fs::path* out) {
  std::error_code ec;
  const fs::path direct = root / fs::u8path(rel);
  if (fs::is_regular_file(direct, ec)) {
    *out = direct;
    return true;
  }
  if (!fs::is_directory(root, ec)) return false;
  fs::path cur = root;
  std::stringstream ss(rel);
  std::string part;
  std::vector<std::string> parts;
  while (std::getline(ss, part, '/')) {
    if (!part.empty()) parts.push_back(part);
  }
  for (const std::string& pt : parts) {
    const fs::path next = cur / fs::u8path(pt);
    if (fs::exists(next, ec)) {
      cur = next;
      continue;
    }
    if (!fs::is_directory(cur, ec)) return false;
    bool found = false;
    const std::string want = lower(pt);
    for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
      if (lower(u8(it->path().filename())) == want) {
        cur = it->path();
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  if (!fs::is_regular_file(cur, ec)) return false;
  *out = cur;
  return true;
}

std::mutex& settings_mutex() {
  static std::mutex m;
  return m;
}
std::vector<std::string>& settings_ref() {
  static std::vector<std::string> v;
  return v;
}

void parse_color(const std::string& s, float* rgb) {
  std::string t;
  for (char c : s) t += (c == '[' || c == ']' || c == '{' || c == '}') ? ' ' : c;
  std::istringstream in(t);
  float v[3];
  if (in >> v[0]) {
    if (!(in >> v[1] >> v[2])) v[1] = v[2] = v[0];
    const bool bytes = s.find('{') != std::string::npos || v[0] > 1.0f || v[1] > 1.0f || v[2] > 1.0f;
    for (int k = 0; k < 3; ++k) rgb[k] = std::clamp(bytes ? v[k] / 255.0f : v[k], 0.0f, 4.0f);
  }
}

#ifdef _WIN32
std::string reg_sz(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[1024];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return u8(fs::path(std::wstring(buf)));
}
#endif

}  // namespace

std::string clean_material_name(const std::string& in) {
  std::string s = in;
  std::replace(s.begin(), s.end(), '\\', '/');
  while (!s.empty() && (s.front() == '/' || s.front() == ' ')) s.erase(s.begin());
  while (!s.empty() && s.back() == ' ') s.pop_back();
  if (lower(s).rfind("materials/", 0) == 0) s = s.substr(10);
  const std::string l = lower(s);
  if (l.size() > 4 && (l.compare(l.size() - 4, 4, ".vmt") == 0 || l.compare(l.size() - 4, 4, ".vtf") == 0)) {
    s.resize(s.size() - 4);
  }
  std::string out;
  for (char c : s) {
    if (c == '/' && !out.empty() && out.back() == '/') continue;
    out += c;
  }
  return out;
}

void set_content_search_folders(const std::vector<std::string>& folders) {
  std::lock_guard<std::mutex> lk(settings_mutex());
  settings_ref() = folders;
}

std::vector<std::string> content_search_folders() {
  std::lock_guard<std::mutex> lk(settings_mutex());
  return settings_ref();
}

std::vector<std::string> split_folder_list(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    std::size_t a = 0, b = cur.size();
    while (a < b && std::isspace(static_cast<unsigned char>(cur[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(cur[b - 1]))) --b;
    std::string t = cur.substr(a, b - a);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
    if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    cur.clear();
  };
  for (char c : text) {
    if (c == ';' || c == '\n' || c == '\r') flush();
    else cur += c;
  }
  flush();
  return out;
}

std::string join_folder_list(const std::vector<std::string>& folders) {
  std::string s;
  for (const std::string& f : folders) s += (s.empty() ? "" : ";") + f;
  return s;
}

std::vector<std::string> detect_content_folders() {
  std::vector<fs::path> steam_roots;
  auto env = [](const char* n) {
    const char* v = std::getenv(n);
    return v ? std::string(v) : std::string();
  };
#ifdef _WIN32
  for (const std::string& sp : {reg_sz(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                                reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
                                reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath")}) {
    if (!sp.empty()) steam_roots.push_back(fs::u8path(sp));
  }
  for (const char* pf : {"ProgramFiles(x86)", "ProgramFiles"}) {
    if (const std::string b = env(pf); !b.empty()) steam_roots.push_back(fs::u8path(b) / "Steam");
  }
#elif defined(__APPLE__)
  if (const std::string h = env("HOME"); !h.empty()) steam_roots.push_back(fs::u8path(h) / "Library/Application Support/Steam");
#else
  if (const std::string h = env("HOME"); !h.empty()) {
    steam_roots.push_back(fs::u8path(h) / ".steam" / "steam");
    steam_roots.push_back(fs::u8path(h) / ".local" / "share" / "Steam");
  }
#endif
  std::vector<fs::path> libs;
  for (const fs::path& r : steam_roots) {
    if (!is_dir(r)) continue;
    libs.push_back(r);
    for (const fs::path& vdf : {r / "steamapps" / "libraryfolders.vdf", r / "config" / "libraryfolders.vdf"}) {
      std::string text;
      if (!read_text(vdf, &text)) continue;
      for (const std::string& l : steam_library_folders_from_vdf(text)) libs.push_back(fs::u8path(l));
    }
  }
  std::vector<std::string> out;
  const char* games[][2] = {{"GarrysMod", "garrysmod"}, {"Half-Life 2", "hl2"}, {"Half-Life 2", "episodic"},
                            {"Counter-Strike Source", "cstrike"}, {"Team Fortress 2", "tf"}};
  for (const fs::path& lib : libs) {
    for (const auto& g : games) {
      const fs::path p = lib / "steamapps" / "common" / g[0] / g[1];
      const std::string s = u8(p.lexically_normal());
      if (is_dir(p) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
  }
  return out;
}

std::string claim_root_of(const std::string& path) {
  std::error_code ec;
  fs::path p = fs::u8path(path);
  if (!fs::is_directory(p, ec)) p = p.parent_path();
  for (int up = 0; up < 32 && !p.empty(); ++up) {
    if (fs::is_regular_file(p / "project.json", ec)) return u8(p);
    const fs::path parent = p.parent_path();
    if (parent == p) break;
    p = parent;
  }
  return {};
}

std::vector<ContentRoot> content_roots(const std::string& model_path, const std::vector<std::string>& material_paths) {
  std::vector<ContentRoot> out;
  std::set<std::string> seen;
  auto add = [&](const fs::path& p, const std::string& why) {
    if (p.empty() || !is_dir(p)) return;
    std::error_code ec;
    fs::path n = fs::weakly_canonical(p, ec);
    if (ec) n = p.lexically_normal();
    std::string key = u8(n);
#ifdef _WIN32
    key = lower(key);
#endif
    if (!seen.insert(key).second) return;
    out.push_back({u8(n), why});
  };
  const fs::path model_dir = fs::u8path(model_path).parent_path();
  for (const std::string& r : material_paths) {
    fs::path p = fs::u8path(r);
    if (p.is_relative()) p = model_dir / p;
    add(p, "material_path setting");
  }
  const std::string claim = claim_root_of(model_path);
  {
    fs::path p = model_dir;
    for (int up = 0; up < 12 && !p.empty(); ++up) {
      if (is_dir(p / "materials")) add(p, "folder above the model");
      if (lower(u8(p.filename())) == "models") add(p.parent_path(), "parent of the model's 'models' folder");
      if (!claim.empty() && u8(p) == claim) break;
      const fs::path parent = p.parent_path();
      if (parent == p) break;
      p = parent;
    }
  }
  if (!claim.empty()) {
    const fs::path c = fs::u8path(claim);
    add(c, "claim");
    add(c / "assets", "claim assets");
    for (const fs::path& d : {c / "assets" / "Models", c / "assets", c / "assets" / "Materials"}) {
      std::error_code ec;
      if (!is_dir(d)) continue;
      std::vector<fs::path> packs;
      for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && is_dir(it->path() / "materials")) packs.push_back(it->path());
      }
      std::sort(packs.begin(), packs.end());
      for (const fs::path& pk : packs) add(pk, "addon / pack in the claim");
    }
    add(c / "assets" / "Materials", "claim Materials folder");
    add(c / "assets" / "Textures", "claim Textures folder");
  }
  for (const std::string& f : content_search_folders()) {
    const fs::path base = fs::u8path(f);
    std::vector<fs::path> games = {base};
    if (is_dir(base / "garrysmod")) games.push_back(base / "garrysmod");
    for (const fs::path& g : games) {
      add(g, "Source content search folder");
      for (const char* sub : {"addons", "download"}) {
        const fs::path d = g / sub;
        if (!is_dir(d)) continue;
        if (is_dir(d / "materials")) add(d, "Source content search folder (" + std::string(sub) + ")");
        std::error_code ec;
        std::vector<fs::path> packs;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
          if (it->is_directory(ec) && is_dir(it->path() / "materials")) packs.push_back(it->path());
        }
        std::sort(packs.begin(), packs.end());
        for (const fs::path& pk : packs) add(pk, "Source content search folder (" + std::string(sub) + ")");
      }
    }
  }
  return out;
}

MaterialResolver::MaterialResolver(std::vector<ContentRoot> roots, std::string model_dir)
    : roots_(std::move(roots)), model_dir_(std::move(model_dir)) {}

bool MaterialResolver::find(const std::string& rel_in, const char* ext, std::string* out) const {
  const std::string rel = clean_material_name(rel_in) + ext;
  fs::path p;
  for (const ContentRoot& r : roots_) {
    const fs::path root = fs::u8path(r.dir);
    if (find_ci(root / "materials", rel, &p) || find_ci(root, rel, &p)) {
      *out = u8(p);
      return true;
    }
  }
  return false;
}

std::shared_ptr<const Image> MaterialResolver::image(const std::string& path, std::string* note) {
  auto it = cache_.find(path);
  if (it != cache_.end()) return it->second;
  std::shared_ptr<const Image> img;
  auto im = std::make_shared<Image>();
  std::string err;
  if (lower(fs::u8path(path).extension().string()) == ".vtf") {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (decode_vtf(b.data(), b.size(), im.get(), &err)) img = std::move(im);
  } else if (load_image_file(path, im.get(), &err)) {
    img = std::move(im);
  }
  if (!img && note) *note += (note->empty() ? "" : "; ") + u8(fs::u8path(path).filename()) + ": " + err;
  cache_[path] = img;
  return img;
}

std::shared_ptr<const Image> MaterialResolver::texture(const std::string& name, const std::string& near_dir,
                                                       std::string* found, std::string* note) {
  std::string p;
  fs::path fp;
  const std::string base = u8(fs::u8path(clean_material_name(name)).filename()) + ".vtf";
  if (find(name, ".vtf", &p) || (!near_dir.empty() && find_ci(fs::u8path(near_dir), base, &fp)) ||
      find_ci(fs::u8path(model_dir_), base, &fp) || find_ci(fs::u8path(model_dir_) / "materials", base, &fp)) {
    if (p.empty()) p = u8(fp);
    if (found) *found = p;
    return image(p, note);
  }
  return nullptr;
}

void MaterialResolver::apply_vmt(const std::string& vmt_path, MaterialDesc* m, MaterialSource* src,
                                 std::string* note) {
  std::string text;
  if (!read_text(fs::u8path(vmt_path), &text)) {
    *note += (note->empty() ? "" : "; ") + std::string("cannot read ") + vmt_path;
    return;
  }
  Vmt vmt;
  std::string err;
  const bool ok = resolve_vmt(
      text,
      [&](const std::string& inc, std::string* t) {
        std::string p;
        return find(inc, ".vmt", &p) && read_text(fs::u8path(p), t);
      },
      &vmt, &err);
  if (!ok) *note += (note->empty() ? "" : "; ") + u8(fs::u8path(vmt_path).filename()) + ": " + err;
  src->shader = vmt.shader;
  const std::string near_dir = u8(fs::u8path(vmt_path).parent_path());
  std::string bt = vmt.param("$basetexture");
  if (bt.empty()) bt = vmt.param("$iris");  // eyes / eyerefract
  std::shared_ptr<const Image> base;
  if (!bt.empty()) {
    base = texture(bt, near_dir, &src->texture, note);
    if (!base) *note += (note->empty() ? "" : "; ") + std::string("$basetexture ") + clean_material_name(bt) + ".vtf not found";
  }
  const std::string bump = vmt.param("$bumpmap").empty() ? vmt.param("$normalmap") : vmt.param("$bumpmap");
  if (!bump.empty() && !vmt.flag("$ssbump")) {
    std::string bf;
    if (auto n = texture(bump, near_dir, &bf, note)) {
      // Source normal maps are Y-down (DirectX); the renderer wants Y-up.
      auto flipped = std::make_shared<Image>(*n);
      for (std::size_t i = 1; i < flipped->rgba.size(); i += 4) flipped->rgba[i] = static_cast<unsigned char>(255 - flipped->rgba[i]);
      m->normal_image = std::move(flipped);
    }
  }
  if (vmt.flag("$nocull")) m->double_sided = true;
  if (!vmt.param("$color2").empty()) parse_color(vmt.param("$color2"), m->base_color);
  else if (!vmt.param("$color").empty()) parse_color(vmt.param("$color"), m->base_color);
  if (vmt.shader == "unlitgeneric" || vmt.shader == "unlittwotexture") m->unlit = true;
  if (vmt.flag("$translucent") || vmt.flag("$additive")) m->alpha_mode = MaterialDesc::kAlphaBlend;
  else if (vmt.flag("$alphatest") || vmt.flag("$allowalphatocoverage")) {
    m->alpha_mode = MaterialDesc::kAlphaMask;
    const std::string ref = vmt.param("$alphatestreference");
    m->alpha_cutoff = ref.empty() ? 0.5f : std::clamp(std::strtof(ref.c_str(), nullptr), 0.01f, 1.0f);
  }
  if (!vmt.param("$alpha").empty()) {
    m->base_color[3] = std::clamp(std::strtof(vmt.param("$alpha").c_str(), nullptr), 0.0f, 1.0f);
    if (m->base_color[3] < 0.999f) m->alpha_mode = MaterialDesc::kAlphaBlend;
  }
  if (base) m->base_color_image = std::move(base);
}

MaterialDesc MaterialResolver::resolve(const std::string& tex, const std::vector<std::string>& cd_paths,
                                       const std::string& override_file, MaterialSource* src) {
  MaterialDesc m;
  m.name = tex;
  m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
  m.roughness = 0.75f;
  MaterialSource local;
  if (!src) src = &local;
  *src = MaterialSource{};
  src->name = tex;
  std::string note;
  const std::string name = clean_material_name(tex);
  std::vector<std::string> names;
  for (const std::string& cd : cd_paths) {
    std::string c = clean_material_name(cd);
    if (!c.empty() && c.back() != '/') c += '/';
    const std::string n = c + name;
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
  }
  if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);

  if (!override_file.empty()) {
    fs::path p = fs::u8path(override_file);
    if (p.is_relative()) p = fs::u8path(model_dir_) / p;
    const std::string ps = u8(p);
    const std::string e = lower(p.extension().string());
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
      src->material = ps;
      src->status = "override";
      if (e == ".vmt") {
        apply_vmt(ps, &m, src, &note);
      } else if (auto img = image(ps, &note)) {
        src->texture = ps;
        m.base_color_image = std::move(img);
      }
    } else {
      note += (note.empty() ? "" : "; ") + std::string("relinked file missing: ") + ps;
    }
  }
  if (src->material.empty()) {
    std::string vmt;
    bool have = false;
    for (const std::string& n : names) {
      if (find(n, ".vmt", &vmt)) {
        have = true;
        break;
      }
    }
    if (!have) {
      fs::path p;
      const std::string flat = u8(fs::u8path(name).filename()) + ".vmt";
      if (find_ci(fs::u8path(model_dir_), flat, &p) || find_ci(fs::u8path(model_dir_) / "materials", flat, &p)) {
        vmt = u8(p);
        have = true;
      }
    }
    if (have) {
      src->material = vmt;
      apply_vmt(vmt, &m, src, &note);
    } else {
      // No .vmt: a bare .vtf of that name still beats the checker.
      for (const std::string& n : names) {
        if (auto img = texture(n, model_dir_, &src->texture, &note)) {
          m.base_color_image = std::move(img);
          src->material = src->texture;
          break;
        }
      }
    }
  }
  if (m.base_color_image) {
    if (src->status.empty()) src->status = "ok";
  } else {
    m.base_color_image = missing_texture_image();
    if (src->status.empty()) src->status = src->material.empty() ? "missing" : "partial";
    std::string looked = "looked for ";
    for (std::size_t i = 0; i < names.size(); ++i) looked += (i ? ", " : "") + std::string("materials/") + names[i] + ".vmt";
    looked += " in ";
    if (roots_.empty()) looked += "(no content folders: add Source content search folders in Settings > Tools)";
    for (std::size_t i = 0; i < roots_.size(); ++i) {
      looked += (i ? "; " : "") + roots_[i].dir + " (" + roots_[i].why + ")";
    }
    if (src->material.empty()) looked += "; also next to the model";
    src->searched = looked;
  }
  if (!note.empty()) src->searched += (src->searched.empty() ? "" : " | ") + note;
  return m;
}

}  // namespace source
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
