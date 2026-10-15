#include "profiler/Profiler.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace profiler {

namespace {

struct Open {
  const char* name;
  double start;
};

struct ThreadStack {
  std::vector<Open> open;
  std::uint32_t id = 0;
};

std::uint32_t thread_number() {
  static std::mutex m;
  static std::unordered_map<std::thread::id, std::uint32_t> ids;
  static std::thread::id main_id = std::this_thread::get_id();
  std::lock_guard<std::mutex> lock(m);
  const auto me = std::this_thread::get_id();
  if (me == main_id) return 0;
  auto it = ids.find(me);
  if (it != ids.end()) return it->second;
  const std::uint32_t n = static_cast<std::uint32_t>(ids.size() + 1);
  ids[me] = n;
  return n;
}

ThreadStack& stack() {
  thread_local ThreadStack s{{}, thread_number()};
  return s;
}

std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.4f", v);
  std::string s = b;
  while (s.size() > 1 && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

}  // namespace

double now_ms() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point t0 = clock::now();
  return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

Profiler& Profiler::get() {
  static Profiler p;
  return p;
}

void Profiler::set_enabled(bool on) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!on) in_frame_ = false;
  enabled_.store(on, std::memory_order_relaxed);
}

void Profiler::set_history(std::size_t frames) {
  std::lock_guard<std::mutex> lock(mu_);
  history_ = std::max<std::size_t>(1, frames);
  if (ring_.size() > history_) ring_.erase(ring_.begin(), ring_.end() - static_cast<long>(history_));
}

void Profiler::begin_frame() {
  if (!enabled()) return;
  std::lock_guard<std::mutex> lock(mu_);
  cur_ = FrameRecord{};
  cur_.index = next_index_++;
  cur_.start_ms = now_ms();
  in_frame_ = true;
}

void Profiler::end_frame() {
  if (!enabled()) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (!in_frame_) return;
  cur_.cpu_ms = now_ms() - cur_.start_ms;
  std::stable_sort(cur_.zones.begin(), cur_.zones.end(), [](const Zone& a, const Zone& b) {
    if (a.thread != b.thread) return a.thread < b.thread;
    if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
    return a.depth < b.depth;
  });
  ring_.push_back(std::move(cur_));
  if (ring_.size() > history_) ring_.erase(ring_.begin(), ring_.begin() + static_cast<long>(ring_.size() - history_));
  cur_ = FrameRecord{};
  in_frame_ = false;
}

void Profiler::begin(const char* name) {
  stack().open.push_back({name ? name : "?", now_ms()});
}

void Profiler::end() {
  ThreadStack& s = stack();
  if (s.open.empty()) return;
  const Open o = s.open.back();
  s.open.pop_back();
  const double t = now_ms();
  std::lock_guard<std::mutex> lock(mu_);
  if (!in_frame_ || o.start < cur_.start_ms) return;
  Zone z;
  z.name = o.name;
  z.depth = static_cast<std::uint16_t>(s.open.size());
  z.thread = s.id;
  z.start_ms = o.start - cur_.start_ms;
  z.dur_ms = t - o.start;
  if (cur_.zones.size() < 20000) cur_.zones.push_back(std::move(z));
}

void Profiler::counter(const std::string& name, double value) {
  if (!enabled()) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (in_frame_) cur_.counters[name] = value;
  else if (!ring_.empty()) ring_.back().counters[name] = value;
}

void Profiler::add(const std::string& name, double value) {
  if (!enabled()) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (in_frame_) cur_.counters[name] += value;
}

void Profiler::gpu(const std::string& pass, double ms) {
  if (!enabled()) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (in_frame_) cur_.gpu_ms[pass] = ms;
  else if (!ring_.empty()) ring_.back().gpu_ms[pass] = ms;
}

std::vector<FrameRecord> Profiler::frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ring_;
}

std::size_t Profiler::frame_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ring_.size();
}

bool Profiler::last(FrameRecord* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (ring_.empty()) return false;
  if (out) *out = ring_.back();
  return true;
}

std::vector<ZoneStat> zone_stats(const std::vector<FrameRecord>& frames, std::size_t last_n) {
  std::map<std::string, ZoneStat> by;
  std::map<std::string, int> frames_with;
  const std::size_t from = (last_n == 0 || last_n >= frames.size()) ? 0 : frames.size() - last_n;
  for (std::size_t f = from; f < frames.size(); ++f) {
    const FrameRecord& fr = frames[f];
    std::map<std::string, bool> seen;
    for (std::size_t i = 0; i < fr.zones.size(); ++i) {
      const Zone& z = fr.zones[i];
      ZoneStat& s = by[z.name];
      s.name = z.name;
      ++s.calls;
      s.total_ms += z.dur_ms;
      s.max_ms = std::max(s.max_ms, z.dur_ms);
      double child = 0.0;
      for (std::size_t j = i + 1; j < fr.zones.size(); ++j) {
        const Zone& c = fr.zones[j];
        if (c.thread != z.thread || c.start_ms >= z.start_ms + z.dur_ms) break;
        if (c.depth == z.depth + 1) child += c.dur_ms;
      }
      s.self_ms += std::max(0.0, z.dur_ms - child);
      seen[z.name] = true;
    }
    for (const auto& [n, _] : seen) ++frames_with[n];
  }
  std::vector<ZoneStat> out;
  for (auto& [n, s] : by) {
    s.avg_ms = frames_with[n] ? s.total_ms / frames_with[n] : 0.0;
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const ZoneStat& a, const ZoneStat& b) {
    if (a.total_ms != b.total_ms) return a.total_ms > b.total_ms;
    return a.name < b.name;
  });
  return out;
}

std::vector<ZoneStat> Profiler::zone_stats(std::size_t last_n) const {
  std::lock_guard<std::mutex> lock(mu_);
  return profiler::zone_stats(ring_, last_n);
}

double Profiler::avg_frame_ms(std::size_t last_n) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (ring_.empty()) return 0.0;
  const std::size_t n = std::min(last_n ? last_n : ring_.size(), ring_.size());
  double t = 0.0;
  for (std::size_t i = ring_.size() - n; i < ring_.size(); ++i) t += ring_[i].cpu_ms;
  return t / static_cast<double>(n);
}

double Profiler::max_frame_ms(std::size_t last_n) const {
  std::lock_guard<std::mutex> lock(mu_);
  double m = 0.0;
  const std::size_t n = std::min(last_n ? last_n : ring_.size(), ring_.size());
  for (std::size_t i = ring_.size() - n; i < ring_.size(); ++i) m = std::max(m, ring_[i].cpu_ms);
  return m;
}

double Profiler::fps(std::size_t last_n) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (ring_.size() < 2) return 0.0;
  const std::size_t n = std::min(std::max<std::size_t>(2, last_n), ring_.size());
  const double span = ring_.back().start_ms - ring_[ring_.size() - n].start_ms;
  return span > 0.0 ? 1000.0 * static_cast<double>(n - 1) / span : 0.0;
}

double Profiler::latest(const std::string& counter, double fallback) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
    const auto c = it->counters.find(counter);
    if (c != it->counters.end()) return c->second;
  }
  return fallback;
}

void Profiler::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  ring_.clear();
}

void Profiler::set_capture_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lock(mu_);
  capture_dir_ = dir;
}

std::string Profiler::capture_dir() const {
  std::lock_guard<std::mutex> lock(mu_);
  return capture_dir_;
}

namespace {

std::string capture_text(const std::vector<FrameRecord>& frames) {
  std::ostringstream o;
  o << "{\"traceEvents\": [\n";
  bool first = true;
  auto sep = [&] {
    if (!first) o << ",\n";
    first = false;
  };
  const double t0 = frames.empty() ? 0.0 : frames.front().start_ms;
  for (std::size_t fi = 0; fi < frames.size(); ++fi) {
    const FrameRecord& f = frames[fi];
    const double base_us = (f.start_ms - t0) * 1000.0;
    sep();
    o << "{\"name\":\"frame " << f.index << "\",\"cat\":\"frame\",\"ph\":\"X\",\"pid\":1,\"tid\":0,\"ts\":"
      << num(base_us) << ",\"dur\":" << num(f.cpu_ms * 1000.0) << "}";
    for (const Zone& z : f.zones) {
      sep();
      o << "{\"name\":\"" << esc(z.name) << "\",\"cat\":\"cpu\",\"ph\":\"X\",\"pid\":1,\"tid\":" << z.thread + 1
        << ",\"ts\":" << num(base_us + z.start_ms * 1000.0) << ",\"dur\":" << num(z.dur_ms * 1000.0)
        << ",\"args\":{\"depth\":" << z.depth << ",\"frame\":" << fi << "}}";
    }
    for (const auto& [k, v] : f.counters) {
      sep();
      o << "{\"name\":\"" << esc(k) << "\",\"ph\":\"C\",\"pid\":1,\"ts\":" << num(base_us)
        << ",\"args\":{\"value\":" << num(v) << "}}";
    }
  }
  o << "\n],\n\"displayTimeUnit\": \"ms\",\n\"tombstone\": {\"format\": 1, \"frames\": [\n";
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const FrameRecord& f = frames[i];
    o << "{\"index\":" << f.index << ",\"start_ms\":" << num(f.start_ms - t0) << ",\"cpu_ms\":" << num(f.cpu_ms)
      << ",\"gpu\":{";
    bool g = true;
    for (const auto& [k, v] : f.gpu_ms) {
      o << (g ? "" : ",") << "\"" << esc(k) << "\":" << num(v);
      g = false;
    }
    o << "},\"counters\":{";
    bool c = true;
    for (const auto& [k, v] : f.counters) {
      o << (c ? "" : ",") << "\"" << esc(k) << "\":" << num(v);
      c = false;
    }
    o << "}}" << (i + 1 < frames.size() ? ",\n" : "\n");
  }
  o << "]}}\n";
  return o.str();
}

}  // namespace

bool Profiler::save_capture(const std::string& path, std::string* error, std::size_t last_n) const {
  std::vector<FrameRecord> fr = frames();
  if (last_n && fr.size() > last_n) fr.erase(fr.begin(), fr.end() - static_cast<long>(last_n));
  if (fr.empty()) {
    if (error) *error = "nothing to capture (profiler off or no frames yet)";
    return false;
  }
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "could not write " + path;
      return false;
    }
    out << capture_text(fr);
    if (!out) {
      if (error) *error = "could not write " + path;
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
  }
  if (ec) {
    if (error) *error = "could not move capture into place: " + ec.message();
    return false;
  }
  return true;
}

std::string Profiler::capture(std::string* error, const std::string& name, std::size_t last_n) const {
  std::string stem = name;
  if (stem.empty()) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[64];
    std::strftime(b, sizeof b, "capture-%Y%m%d-%H%M%S", &tm);
    stem = b;
  }
  for (char& c : stem) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) c = '_';
  }
  const std::string path = (fs::path(capture_dir()) / (stem + ".tsprof.json")).string();
  return save_capture(path, error, last_n) ? path : std::string();
}

bool parse_capture(const std::string& text, std::vector<FrameRecord>* frames, std::string* error) {
  json::Value root;
  if (!json::parse(text, &root, error)) return false;
  const json::Value& ts = root["tombstone"];
  if (!ts.is_object() || ts.num("format", 0) != 1) {
    if (error) *error = "not a TombStone capture (no tombstone.format 1)";
    return false;
  }
  std::vector<FrameRecord> out;
  for (const json::Value& f : ts["frames"].items()) {
    FrameRecord r;
    r.index = static_cast<std::uint64_t>(f.num("index", 0));
    r.start_ms = f.num("start_ms", 0);
    r.cpu_ms = f.num("cpu_ms", 0);
    for (const auto& [k, v] : f["gpu"].members()) r.gpu_ms[k] = v.as_number();
    for (const auto& [k, v] : f["counters"].members()) r.counters[k] = v.as_number();
    out.push_back(std::move(r));
  }
  // Zones: CPU "X" events, assigned to the frame whose span holds them.
  std::size_t fi = 0;
  for (const json::Value& e : root["traceEvents"].items()) {
    if (e.str("ph") != "X" || e.str("cat") != "cpu" || out.empty()) continue;
    const double ts_ms = e.num("ts", 0) / 1000.0;
    const double tagged = e["args"].num("frame", -1);
    if (tagged >= 0 && tagged < static_cast<double>(out.size())) {
      fi = static_cast<std::size_t>(tagged);  // the frame the zone was recorded in
    } else {  // a foreign trace: by time
      while (fi + 1 < out.size() && ts_ms >= out[fi + 1].start_ms) ++fi;
      while (fi > 0 && ts_ms < out[fi].start_ms) --fi;
    }
    Zone z;
    z.name = e.str("name");
    z.thread = static_cast<std::uint32_t>(std::max(0.0, e.num("tid", 1) - 1));
    z.depth = static_cast<std::uint16_t>(e["args"].num("depth", 0));
    z.start_ms = ts_ms - out[fi].start_ms;
    z.dur_ms = e.num("dur", 0) / 1000.0;
    out[fi].zones.push_back(std::move(z));
  }
  if (frames) *frames = std::move(out);
  return true;
}

bool load_capture(const std::string& path, std::vector<FrameRecord>* frames, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "could not open " + path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_capture(ss.str(), frames, error);
}

double process_memory_mb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
    return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
  }
  return 0.0;
#elif defined(__linux__)
  std::ifstream f("/proc/self/statm");
  long pages = 0, resident = 0;
  if (f >> pages >> resident) {
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
  }
  return 0.0;
#else
  return 0.0;
#endif
}

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
