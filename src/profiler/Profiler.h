#pragma once

// Assay Office core (ts_profiler): a small frame profiler every host can
// feed. Off by default and close to free while off (one relaxed load per
// scope). The optional ts.profiler plugin, the editor panels and the
// in-game debug tools switch it on.
//
//   TS_PROFILE_SCOPE("world.step");          // named CPU scope (RAII)
//   Profiler::get().counter("draws", 31);    // per-frame counter
//   Profiler::get().gpu("opaque", 1.8);      // GPU pass time (ms)
//
// Hosts call begin_frame() / end_frame() once per frame. The last
// history() frames are kept: each has its zones (name, depth, thread,
// start, duration), counters and GPU timings. Captures are Chrome trace
// event JSON (open in Perfetto or chrome://tracing) plus a "tombstone"
// block the Assay Office panel reads back:
//   <project>/captures/<name>.tsprof.json

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace profiler {

// Milliseconds on a steady clock (arbitrary epoch, process lifetime).
double now_ms();

struct Zone {
  std::string name;
  std::uint16_t depth = 0;   // 0 = top level on its thread
  std::uint32_t thread = 0;  // 0 = the thread that runs frames
  double start_ms = 0.0;     // relative to the frame start
  double dur_ms = 0.0;
};

struct FrameRecord {
  std::uint64_t index = 0;
  double start_ms = 0.0;  // now_ms() at begin_frame
  double cpu_ms = 0.0;    // begin_frame -> end_frame
  std::vector<Zone> zones;
  std::map<std::string, double> counters;  // "draws", "mem.process_mb", "net.rtt_ms", ...
  std::map<std::string, double> gpu_ms;    // GPU pass -> ms
};

struct ZoneStat {
  std::string name;
  int calls = 0;
  double total_ms = 0.0;
  double avg_ms = 0.0;  // per frame it appeared in
  double max_ms = 0.0;  // worst single call
  double self_ms = 0.0; // total minus child zones
};

class Profiler {
 public:
  static Profiler& get();

  void set_enabled(bool on);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  // Frames kept (default 600 = 10 s at 60 Hz).
  void set_history(std::size_t frames);
  std::size_t history() const { return history_; }

  void begin_frame();
  void end_frame();
  bool in_frame() const { return in_frame_; }

  // Scope push / pop on the calling thread (use TS_PROFILE_SCOPE).
  void begin(const char* name);
  void end();

  void counter(const std::string& name, double value);  // set for this frame
  void add(const std::string& name, double value);      // accumulate this frame
  void gpu(const std::string& pass, double ms);

  std::vector<FrameRecord> frames() const;
  std::size_t frame_count() const;
  bool last(FrameRecord* out) const;  // newest finished frame
  // Per-name totals over the newest `last_n` frames (0 = all), slowest first.
  std::vector<ZoneStat> zone_stats(std::size_t last_n = 0) const;
  double avg_frame_ms(std::size_t last_n = 60) const;
  double max_frame_ms(std::size_t last_n = 60) const;
  double fps(std::size_t last_n = 60) const;  // from frame start spacing
  // Latest value of a counter (newest frame that has it).
  double latest(const std::string& counter, double fallback = 0.0) const;
  void clear();

  // Captures.
  void set_capture_dir(const std::string& dir);
  std::string capture_dir() const;
  // Write the newest `last_n` frames (0 = all) to `path`.
  bool save_capture(const std::string& path, std::string* error, std::size_t last_n = 0) const;
  // <capture_dir>/<name or timestamp>.tsprof.json; returns the path ("" on error).
  std::string capture(std::string* error, const std::string& name = {}, std::size_t last_n = 0) const;

 private:
  Profiler() = default;
  std::atomic<bool> enabled_{false};
  mutable std::mutex mu_;
  std::vector<FrameRecord> ring_;  // oldest first
  std::size_t history_ = 600;
  FrameRecord cur_;
  bool in_frame_ = false;
  std::uint64_t next_index_ = 1;
  std::string capture_dir_ = "captures";
};

// Read a capture written by save_capture (frames oldest first).
bool load_capture(const std::string& path, std::vector<FrameRecord>* frames, std::string* error);
// Same, from text.
bool parse_capture(const std::string& text, std::vector<FrameRecord>* frames, std::string* error);
// Stats over a frame list (the panel uses it for loaded captures too).
std::vector<ZoneStat> zone_stats(const std::vector<FrameRecord>& frames, std::size_t last_n = 0);

// Resident memory of this process in MB (0 when the platform can't say).
double process_memory_mb();

class Scope {
 public:
  explicit Scope(const char* name) {
    if (Profiler::get().enabled()) {
      on_ = true;
      Profiler::get().begin(name);
    }
  }
  ~Scope() {
    if (on_) Profiler::get().end();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool on_ = false;
};

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts

#define TS_PROF_CAT2(a, b) a##b
#define TS_PROF_CAT(a, b) TS_PROF_CAT2(a, b)
#define TS_PROFILE_SCOPE(name) ::ts::tombstone::profiler::Scope TS_PROF_CAT(ts_prof_scope_, __LINE__)(name)
