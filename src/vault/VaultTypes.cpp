#include "vault/VaultTypes.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>

namespace ts {
namespace tombstone {
namespace vault {

using json::Value;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains(const std::string& hay, const std::string& needle) {
  return lower(hay).find(needle) != std::string::npos;
}

}  // namespace

const Version* Item::version(int v) const {
  if (versions.empty()) return nullptr;
  if (v <= 0) v = latest > 0 ? latest : versions.back().version;
  for (const Version& x : versions) {
    if (x.version == v) return &x;
  }
  return nullptr;
}

const FileRec* Item::main_file(int v) const {
  const Version* ver = version(v);
  if (!ver) return nullptr;
  for (const FileRec& f : ver->files) {
    if (f.role == "main") return &f;
  }
  return nullptr;
}

bool item_matches(const Item& it, const Query& q) {
  if (!q.kind.empty() && it.kind != q.kind) return false;
  if (!q.dim.empty() && std::find(it.dims.begin(), it.dims.end(), lower(q.dim)) == it.dims.end()) return false;
  if (!q.tag.empty() && std::find(it.tags.begin(), it.tags.end(), lower(q.tag)) == it.tags.end()) return false;
  if (q.text.empty()) return true;
  const std::string t = lower(q.text);
  if (contains(it.name, t) || contains(it.author, t) || contains(it.description, t) ||
      contains(it.licence, t) || it.id.rfind(t, 0) == 0) {
    return true;
  }
  for (const std::string& tag : it.tags) {
    if (contains(tag, t)) return true;
  }
  return false;
}

Value strings_json(const std::vector<std::string>& v) {
  Value a = Value::array();
  for (const std::string& s : v) a.push(Value::string(s));
  return a;
}

std::vector<std::string> json_strings(const Value& j) {
  std::vector<std::string> out;
  if (!j.is_array()) return out;
  for (const Value& v : j.items()) {
    if (v.is_string()) out.push_back(v.as_string());
  }
  return out;
}

Value to_json(const FileRec& f) {
  Value o = Value::object();
  o.set("path", Value::string(f.path));
  o.set("sha256", Value::string(f.sha256));
  o.set("size", Value::number(static_cast<double>(f.size)));
  o.set("role", Value::string(f.role));
  if (!f.guid.empty()) o.set("guid", Value::string(f.guid));
  return o;
}

Value to_json(const Version& v, bool with_files) {
  Value o = Value::object();
  o.set("version", Value::number(v.version));
  o.set("created", Value::string(v.created));
  o.set("note", Value::string(v.note));
  o.set("uploader", Value::string(v.uploader));
  o.set("size", Value::number(static_cast<double>(v.size)));
  o.set("thumbnail", Value::string(v.thumbnail));
  if (with_files) {
    Value files = Value::array();
    for (const FileRec& f : v.files) files.push(to_json(f));
    o.set("files", files);
  }
  Value deps = Value::array();
  for (const Dep& d : v.deps) {
    Value x = Value::object();
    x.set("item", Value::string(d.item));
    x.set("version", Value::number(d.version));
    deps.push(x);
  }
  o.set("deps", deps);
  return o;
}

Value to_json(const Item& it, bool with_versions, bool with_files) {
  Value o = Value::object();
  o.set("id", Value::string(it.id));
  o.set("name", Value::string(it.name));
  o.set("kind", Value::string(it.kind));
  o.set("author", Value::string(it.author));
  o.set("licence", Value::string(it.licence));
  o.set("description", Value::string(it.description));
  o.set("tags", strings_json(it.tags));
  o.set("dims", strings_json(it.dims));
  o.set("thumbnail", Value::string(it.thumbnail));
  o.set("latest", Value::number(it.latest));
  o.set("created", Value::string(it.created));
  o.set("updated", Value::string(it.updated));
  if (with_versions) {
    Value vs = Value::array();
    for (const Version& v : it.versions) vs.push(to_json(v, with_files));
    o.set("versions", vs);
  }
  return o;
}

bool from_json(const Value& j, FileRec* f) {
  if (!j.is_object()) return false;
  f->path = j.str("path");
  f->sha256 = j.str("sha256");
  f->size = static_cast<std::uint64_t>(j.num("size", 0));
  f->role = j.str("role", "file");
  f->guid = j.str("guid");
  return !f->path.empty() && !f->sha256.empty();
}

bool from_json(const Value& j, Version* v) {
  if (!j.is_object()) return false;
  v->version = static_cast<int>(j.num("version", 0));
  v->created = j.str("created");
  v->note = j.str("note");
  v->uploader = j.str("uploader");
  v->size = static_cast<std::uint64_t>(j.num("size", 0));
  v->thumbnail = j.str("thumbnail");
  v->files.clear();
  v->deps.clear();
  for (const Value& f : j["files"].items()) {
    FileRec r;
    if (from_json(f, &r)) v->files.push_back(r);
  }
  for (const Value& d : j["deps"].items()) {
    Dep x;
    x.item = d.str("item");
    x.version = static_cast<int>(d.num("version", 0));
    if (!x.item.empty()) v->deps.push_back(x);
  }
  return true;
}

bool from_json(const Value& j, Item* it) {
  if (!j.is_object()) return false;
  it->id = j.str("id");
  it->name = j.str("name");
  it->kind = j.str("kind");
  it->author = j.str("author");
  it->licence = j.str("licence");
  it->description = j.str("description");
  it->tags = json_strings(j["tags"]);
  it->dims = json_strings(j["dims"]);
  it->thumbnail = j.str("thumbnail");
  it->latest = static_cast<int>(j.num("latest", 0));
  it->created = j.str("created");
  it->updated = j.str("updated");
  it->versions.clear();
  for (const Value& v : j["versions"].items()) {
    Version x;
    if (from_json(v, &x)) it->versions.push_back(x);
  }
  return !it->id.empty();
}

bool valid_item_id(const std::string& id) {
  if (id.size() != 32) return false;
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::vector<std::string> normalise_dims(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  for (const char* d : {"2d", "3d", "vr"}) {
    for (const std::string& s : in) {
      if (lower(s) == d) {
        out.push_back(d);
        break;
      }
    }
  }
  return out;
}

std::vector<std::string> split_tags(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    std::size_t a = 0, b = cur.size();
    while (a < b && std::isspace(static_cast<unsigned char>(cur[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(cur[b - 1]))) --b;
    std::string t = lower(cur.substr(a, b - a));
    if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    cur.clear();
  };
  for (char c : s) {
    if (c == ',' || c == ';') flush();
    else cur += c;
  }
  flush();
  return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::string utc_now_iso() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

bool safe_rel_path(const std::string& rel) {
  if (rel.empty() || rel.size() > 1024) return false;
  if (rel[0] == '/' || rel[0] == '\\') return false;
  if (rel.find(':') != std::string::npos || rel.find('\\') != std::string::npos) return false;
  std::size_t start = 0;
  while (start <= rel.size()) {
    const std::size_t slash = rel.find('/', start);
    const std::string seg = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
