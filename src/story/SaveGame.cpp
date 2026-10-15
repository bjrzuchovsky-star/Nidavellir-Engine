#include "story/SaveGame.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace story {

namespace {

constexpr const char* kExt = ".save.json";

std::string iso_now(long long* stamp) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  *stamp = static_cast<long long>(t);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

bool read_file(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

SlotInfo info_of(const std::string& slot, const json::Value& v) {
  SlotInfo i;
  i.slot = slot;
  i.label = v.str("label");
  i.time = v.str("time");
  i.playtime = v.num("playtime", 0);
  i.scene = v.str("scene");
  i.autosave = v.flag("auto", false);
  i.stamp = static_cast<long long>(v.num("stamp", 0));
  return i;
}

}  // namespace

bool valid_slot_name(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
  }
  return true;
}

SaveStore::SaveStore(std::string project_dir) : dir_(std::move(project_dir)) {}

std::string SaveStore::slots_dir() const { return (fs::path(dir_) / "saves" / "slots").string(); }

std::string SaveStore::path(const std::string& slot) const {
  return (fs::path(slots_dir()) / (slot + kExt)).string();
}

void SaveStore::provide(Provider p) {
  for (Provider& x : providers_) {
    if (x.section == p.section) {
      x = std::move(p);
      return;
    }
  }
  providers_.push_back(std::move(p));
}

std::vector<std::string> SaveStore::sections() const {
  std::vector<std::string> out;
  for (const Provider& p : providers_) out.push_back(p.section);
  return out;
}

bool SaveStore::write(const std::string& slot, const std::string& label, bool autosave, std::string* error) {
  if (!valid_slot_name(slot)) {
    if (error) *error = "bad slot name \"" + slot + "\" (letters, digits, - and _)";
    return false;
  }
  long long stamp = 0;
  json::Value v = json::Value::object();
  v.set("format", json::Value::number(kFormat));
  v.set("slot", json::Value::string(slot));
  v.set("label", json::Value::string(label));
  v.set("time", json::Value::string(iso_now(&stamp)));
  // Same-second saves still sort: stamp grows past the newest on disk.
  long long newest = 0;
  for (const SlotInfo& s : list()) newest = std::max(newest, s.stamp);
  if (stamp <= newest) stamp = newest + 1;
  v.set("stamp", json::Value::number(static_cast<double>(stamp)));
  v.set("playtime", json::Value::number(playtime));
  v.set("scene", json::Value::string(scene));
  v.set("auto", json::Value::boolean(autosave));
  json::Value sec = json::Value::object();
  for (const Provider& p : providers_) {
    if (p.save) sec.set(p.section, p.save());
  }
  v.set("sections", sec);
  std::error_code ec;
  fs::create_directories(slots_dir(), ec);
  const fs::path final_path = path(slot);
  const fs::path tmp = final_path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
    f << json::write(v);
    if (!f.good()) {
      if (error) *error = "write failed: " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, final_path, ec);
  if (ec) {
    // Windows will not rename over an existing file on every filesystem.
    fs::remove(final_path, ec);
    ec.clear();
    fs::rename(tmp, final_path, ec);
    if (ec) {
      if (error) *error = "cannot replace " + final_path.string() + ": " + ec.message();
      return false;
    }
  }
  ++written_;
  return true;
}

bool SaveStore::save(const std::string& slot, const std::string& label, std::string* error) {
  if (slot.rfind("auto-", 0) == 0) {
    if (error) *error = "auto-* slots belong to autosave";
    return false;
  }
  return write(slot, label, false, error);
}

bool SaveStore::autosave(std::string* slot_out, std::string* error) {
  const int n = std::max(1, autosave_slots);
  std::string pick;
  long long oldest = 0;
  for (int i = 1; i <= n; ++i) {
    const std::string s = "auto-" + std::to_string(i);
    SlotInfo in;
    if (!info(s, &in)) {
      pick = s;
      break;
    }
    if (pick.empty() || in.stamp < oldest) {
      pick = s;
      oldest = in.stamp;
    }
  }
  if (slot_out) *slot_out = pick;
  return write(pick, "Autosave", true, error);
}

bool SaveStore::load(const std::string& slot, std::string* error) {
  std::string text;
  if (!valid_slot_name(slot) || !read_file(path(slot), &text)) {
    if (error) *error = "no save in slot \"" + slot + "\"";
    return false;
  }
  json::Value v;
  std::string err;
  if (!json::parse(text, &v, &err)) {
    if (error) *error = "slot " + slot + " is damaged: " + err;
    return false;
  }
  const int format = static_cast<int>(v.num("format", 0));
  if (format < 1 || format > kFormat) {
    if (error) *error = "slot " + slot + " has save format " + std::to_string(format) + " (this build reads 1.." +
                        std::to_string(kFormat) + ")";
    return false;
  }
  playtime = v.num("playtime", playtime);
  const json::Value& sec = v["sections"];
  for (const Provider& p : providers_) {
    // A section missing from an older save loads as empty.
    if (p.load) p.load(sec.has(p.section) ? sec[p.section] : json::Value::object());
  }
  ++loaded_;
  return true;
}

bool SaveStore::remove(const std::string& slot) {
  if (!valid_slot_name(slot)) return false;
  std::error_code ec;
  return fs::remove(path(slot), ec);
}

bool SaveStore::exists(const std::string& slot) const {
  std::error_code ec;
  return valid_slot_name(slot) && fs::exists(path(slot), ec);
}

bool SaveStore::info(const std::string& slot, SlotInfo* out) const {
  std::string text;
  if (!valid_slot_name(slot) || !read_file(path(slot), &text)) return false;
  json::Value v;
  if (!json::parse(text, &v, nullptr)) return false;
  *out = info_of(slot, v);
  return true;
}

std::vector<SlotInfo> SaveStore::list() const {
  std::vector<SlotInfo> out;
  std::error_code ec;
  if (!fs::is_directory(slots_dir(), ec)) return out;
  for (const auto& e : fs::directory_iterator(slots_dir(), ec)) {
    const std::string n = e.path().filename().string();
    const std::size_t el = std::string(kExt).size();
    if (n.size() <= el || n.substr(n.size() - el) != kExt) continue;
    SlotInfo i;
    if (info(n.substr(0, n.size() - el), &i)) out.push_back(std::move(i));
  }
  std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b) {
    return a.stamp != b.stamp ? a.stamp > b.stamp : a.slot < b.slot;
  });
  return out;
}

std::string SaveStore::latest() const {
  const auto l = list();
  return l.empty() ? std::string() : l.front().slot;
}

}  // namespace story
}  // namespace tombstone
}  // namespace ts
