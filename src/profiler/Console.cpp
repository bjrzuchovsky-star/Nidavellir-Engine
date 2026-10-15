#include "profiler/Console.h"

#include "profiler/Profiler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ts {
namespace tombstone {
namespace profiler {

namespace {
std::string fmt(const char* f, double a) {
  char b[64];
  std::snprintf(b, sizeof b, f, a);
  return b;
}
}  // namespace

std::vector<std::string> split_args(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (char c : line) {
    if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if (!quoted && (c == ' ' || c == '\t')) {
      if (any) out.push_back(cur);
      cur.clear();
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

Console::Console() {
  add("help", "list commands (help <command> for one)", [this](const std::vector<std::string>& a) {
    std::string o;
    if (a.size() > 1) {
      const auto it = cmds_.find(a[1]);
      return it == cmds_.end() ? "no command '" + a[1] + "'" : a[1] + ": " + it->second.help;
    }
    for (const auto& [n, c] : cmds_) o += (o.empty() ? "" : "\n") + n + "  " + c.help;
    return o;
  });
  add("echo", "print the rest of the line", [](const std::vector<std::string>& a) {
    std::string o;
    for (std::size_t i = 1; i < a.size(); ++i) o += (i > 1 ? " " : "") + a[i];
    return o;
  });
  add("prof", "prof on|off|clear: switch the profiler, drop its history", [](const std::vector<std::string>& a) {
    Profiler& p = Profiler::get();
    const std::string v = a.size() > 1 ? a[1] : "";
    if (v == "on") p.set_enabled(true);
    else if (v == "off") p.set_enabled(false);
    else if (v == "clear") p.clear();
    else if (!v.empty()) return std::string("usage: prof on|off|clear");
    return std::string("profiler ") + (p.enabled() ? "on" : "off") + ", " + std::to_string(p.frame_count()) +
           " frames kept";
  });
  add("capture", "capture [name] [frames]: save the profile history to a .tsprof.json",
      [](const std::vector<std::string>& a) {
        std::string err;
        const std::size_t n = a.size() > 2 ? static_cast<std::size_t>(std::atoi(a[2].c_str())) : 0;
        const std::string path = Profiler::get().capture(&err, a.size() > 1 ? a[1] : "", n);
        return path.empty() ? "capture failed: " + err : "capture saved: " + path;
      });
  add("fps", "frame rate, average and worst frame (last 60)", [](const std::vector<std::string>&) {
    Profiler& p = Profiler::get();
    if (!p.enabled()) return std::string("profiler is off (prof on)");
    return fmt("%.1f fps", p.fps()) + fmt(", avg %.2f ms", p.avg_frame_ms()) + fmt(", worst %.2f ms", p.max_frame_ms());
  });
  add("scopes", "scopes [n]: slowest named scopes over the last 60 frames", [](const std::vector<std::string>& a) {
    const int n = a.size() > 1 ? std::max(1, std::atoi(a[1].c_str())) : 8;
    const auto st = Profiler::get().zone_stats(60);
    if (st.empty()) return std::string("no scopes recorded (prof on, then play a little)");
    std::string o;
    for (int i = 0; i < n && i < static_cast<int>(st.size()); ++i) {
      char b[160];
      std::snprintf(b, sizeof b, "%-28s avg %7.3f ms  max %7.3f ms  calls %d", st[i].name.c_str(), st[i].avg_ms,
                    st[i].max_ms, st[i].calls);
      o += (o.empty() ? "" : "\n") + std::string(b);
    }
    return o;
  });
  add("counters", "counters [prefix]: latest counters", [](const std::vector<std::string>& a) {
    FrameRecord f;
    if (!Profiler::get().last(&f)) return std::string("no frames yet");
    const std::string pre = a.size() > 1 ? a[1] : "";
    std::string o;
    for (const auto& [k, v] : f.counters) {
      if (k.rfind(pre, 0) != 0) continue;
      o += (o.empty() ? "" : "\n") + k + " = " + fmt("%g", v);
    }
    return o.empty() ? std::string("no counters") : o;
  });
  add("gpu", "GPU pass timings (needs a GL renderer)", [](const std::vector<std::string>&) {
    FrameRecord f;
    if (!Profiler::get().last(&f) || f.gpu_ms.empty()) return std::string("no GPU timings (null renderer, or prof off)");
    std::string o;
    double total = 0;
    for (const auto& [k, v] : f.gpu_ms) {
      o += (o.empty() ? "" : "\n") + k + fmt("  %.3f ms", v);
      total += v;
    }
    return o + fmt("\ntotal  %.3f ms", total);
  });
  add("mem", "memory counters", [](const std::vector<std::string>&) {
    std::string o = fmt("process %.1f MB", process_memory_mb());
    FrameRecord f;
    if (Profiler::get().last(&f)) {
      for (const auto& [k, v] : f.counters) {
        if (k.rfind("mem.", 0) == 0 && k != "mem.process_mb") o += "\n" + k + " = " + fmt("%g", v);
      }
    }
    return o;
  });
}

void Console::add(const std::string& name, const std::string& help, Fn fn) {
  cmds_[name] = Cmd{help, std::move(fn)};
}

void Console::remove(const std::string& name) { cmds_.erase(name); }

Console& Console::shared() {
  static Console c;
  return c;
}

bool Console::has(const std::string& name) const { return cmds_.count(name) != 0; }

std::vector<std::string> Console::names() const {
  std::vector<std::string> out;
  for (const auto& [n, _] : cmds_) out.push_back(n);
  return out;
}

void Console::print(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    log_.push_back(line);
    while (log_.size() > kMaxLog) log_.pop_front();
  }
}

std::string Console::run(const std::string& line) {
  const std::vector<std::string> a = split_args(line);
  if (a.empty()) return {};
  print("> " + line);
  const auto it = cmds_.find(a[0]);
  std::string out;
  if (it == cmds_.end()) {
    out = "unknown command '" + a[0] + "' (try help)";
  } else {
    try {
      out = it->second.fn(a);
    } catch (const std::exception& e) {
      out = a[0] + " failed: " + e.what();
    }
  }
  if (!out.empty()) print(out);
  return out;
}

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
