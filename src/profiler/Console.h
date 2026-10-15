#pragma once

// Debug console commands (ts_profiler). One registry per host: the in-game
// console (backtick), the editor Telegraph's command line and Lua
// `console.run(line)` all go through Console::run.
//
//   help [command]          list commands
//   prof on|off|clear       profiler on / off, drop the history
//   capture [name] [frames] save a capture (<capture dir>/<name>.tsprof.json)
//   fps                     frame rate, average / worst frame
//   scopes [n]              slowest named scopes over the last 60 frames
//   counters [prefix]       latest counters (draws, mem.*, net.*, assets.*)
//   gpu                     GPU pass timings
//   mem                     memory counters
//   echo text               print text
// Hosts add their own (overlay, pause, timescale, reload, ...).

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace profiler {

class Console {
 public:
  // args[0] is the command name.
  using Fn = std::function<std::string(const std::vector<std::string>& args)>;
  static constexpr std::size_t kMaxLog = 200;

  Console();  // with the built-in commands above
  // The process-wide console the hosts share (editor Telegraph, in-game
  // console, Lua console.run).
  static Console& shared();
  void add(const std::string& name, const std::string& help, Fn fn);
  void remove(const std::string& name);
  bool has(const std::string& name) const;
  std::vector<std::string> names() const;
  // Run one line; the output (possibly several lines) is returned and
  // also appended to log() after a "> line" echo.
  std::string run(const std::string& line);
  void print(const std::string& text);
  const std::deque<std::string>& log() const { return log_; }
  void clear_log() { log_.clear(); }

 private:
  struct Cmd {
    std::string help;
    Fn fn;
  };
  std::map<std::string, Cmd> cmds_;
  std::deque<std::string> log_;
};

std::vector<std::string> split_args(const std::string& line);

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
