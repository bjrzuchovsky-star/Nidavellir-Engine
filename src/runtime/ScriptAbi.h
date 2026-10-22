#pragma once

// The C ABI (sdk/include/tombstone/script_abi.h) over a ScriptWorld, shared
// by compiled script languages (C++ modules, C#), plus a tiny dynamic
// library loader and process runner they both use.

#include "runtime/Script.h"
#include "tombstone/script_abi.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

TsScriptApi make_script_api(ScriptWorld* world);
// "name\ttype\tvalue\n" lines (type n / b / t).
std::string encode_script_props(const std::vector<ScriptProp>& props);

class SharedLib {
 public:
  SharedLib() = default;
  ~SharedLib();
  SharedLib(const SharedLib&) = delete;
  SharedLib& operator=(const SharedLib&) = delete;
  bool open(const std::string& path, std::string* error);
  void close();
  void* symbol(const char* name) const;
  bool is_open() const { return h_ != nullptr; }
  static const char* extension();  // ".dll" / ".so" / ".dylib"

 private:
  void* h_ = nullptr;
};

// Runs a shell command, output (stdout + stderr) into *output. Exit code.
int run_command(const std::string& command, std::string* output);
// Telegraph entries from compiler output: gcc / clang "file:line:col: error:"
// and MSVC "file(line): error C1234:" lines (errors and warnings).
std::vector<LogEntry> parse_build_output(const std::string& output, const std::string& project_dir);
// `file` made relative to `project_dir` when it lies inside it (generic
// slashes). Both are canonicalized first: long names for 8.3 short names
// and a case-insensitive compare on Windows, symlinks / ".." everywhere.
std::string project_relative_path(const std::string& file, const std::string& project_dir);

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
