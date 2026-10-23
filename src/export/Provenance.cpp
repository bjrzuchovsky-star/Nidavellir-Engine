#include "export/Provenance.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ts {
namespace tombstone {
namespace exporter {
namespace provenance {

namespace fs = std::filesystem;
using json::Value;

namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

std::string now_stamp() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  return buf;
}

bool load(const std::string& claim_dir, Ledger* out) {
  *out = Ledger{};
  std::ifstream in(fs::u8path(claim_dir) / kLedgerFile, std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Value root;
  std::string err;
  if (!json::parse(text, &root, &err) || !root.is_object()) return false;
  for (const auto& [k, v] : root["assets"].members()) {
    Record r;
    r.origin = v.str("origin");
    r.format = v.str("format");
    r.author = v.str("author");
    r.addon = v.str("addon");
    r.source = v.str("source");
    r.imported = v.str("imported");
    out->assets[k] = r;
  }
  for (const auto& [k, v] : root["acknowledged"].members()) out->acknowledged[k] = v.as_string_or("yes");
  out->acknowledged_all = root.str("acknowledged_all");
  return true;
}

bool save(const std::string& claim_dir, const Ledger& l, std::string* error) {
  Value root = Value::object();
  root.set("provenance", Value::number(1));
  root.set("note", Value::string("Third-party content imported into this claim. You need the rights to ship it."));
  Value assets = Value::object();
  for (const auto& [k, r] : l.assets) {
    Value v = Value::object();
    v.set("origin", Value::string(r.origin));
    if (!r.format.empty()) v.set("format", Value::string(r.format));
    if (!r.author.empty()) v.set("author", Value::string(r.author));
    if (!r.addon.empty()) v.set("addon", Value::string(r.addon));
    if (!r.source.empty()) v.set("source", Value::string(r.source));
    if (!r.imported.empty()) v.set("imported", Value::string(r.imported));
    assets.set(k, v);
  }
  root.set("assets", assets);
  Value acks = Value::object();
  for (const auto& [k, w] : l.acknowledged) acks.set(k, Value::string(w));
  root.set("acknowledged", acks);
  root.set("acknowledged_all", Value::string(l.acknowledged_all));
  const fs::path p = fs::u8path(claim_dir) / kLedgerFile;
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << json::write(root);
  if (!out) {
    if (error) *error = "cannot write " + p.string();
    return false;
  }
  return true;
}

bool tag(const std::string& claim_dir, const std::map<std::string, Record>& records, std::string* error) {
  Ledger l;
  load(claim_dir, &l);
  for (const auto& [k, r] : records) l.assets[k] = r;
  return save(claim_dir, l, error);
}

bool acknowledge(const std::string& claim_dir, const std::string& rel, bool on, std::string* error) {
  Ledger l;
  load(claim_dir, &l);
  if (rel.empty()) {
    l.acknowledged_all = on ? now_stamp() : std::string();
  } else if (on) {
    l.acknowledged[rel] = now_stamp();
  } else {
    l.acknowledged.erase(rel);
  }
  return save(claim_dir, l, error);
}

bool is_source_format(const std::string& rel) {
  const std::string r = lower(rel);
  for (const char* e : {".mdl", ".vvd", ".vtx", ".phy", ".ani", ".vtf", ".vmt", ".gma"}) {
    const std::size_t n = std::char_traits<char>::length(e);
    if (r.size() > n && r.compare(r.size() - n, n, e) == 0) return true;
  }
  return false;
}

std::vector<Shipped> shipped_third_party(const std::string& claim_dir, const std::vector<std::string>& content) {
  Ledger l;
  load(claim_dir, &l);
  std::vector<Shipped> out;
  for (const std::string& rel : content) {
    auto it = l.assets.find(rel);
    if (it == l.assets.end() && !is_source_format(rel)) continue;
    Shipped s;
    s.path = rel;
    s.origin = it != l.assets.end() ? it->second.origin : std::string("source-engine");
    if (it != l.assets.end()) s.author = it->second.author;
    s.acknowledged = !l.acknowledged_all.empty() || l.acknowledged.count(rel) > 0;
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const Shipped& a, const Shipped& b) { return a.path < b.path; });
  return out;
}

}  // namespace provenance
}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
