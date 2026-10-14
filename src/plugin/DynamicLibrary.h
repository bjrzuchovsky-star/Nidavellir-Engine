#pragma once

// Thin LoadLibraryW / dlopen wrapper. Closing happens in the destructor.

#include <string>

namespace ts {
namespace tombstone {
namespace plugin {

class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  bool open(const std::string& path, std::string* error);
  void* symbol(const char* name, std::string* error) const;
  void close();
  bool is_open() const { return handle_ != nullptr; }
  const std::string& path() const { return path_; }

  // ".dll" on Windows, ".dylib" on macOS, ".so" elsewhere.
  static const char* native_extension();

 private:
  void* handle_ = nullptr;
  std::string path_;
};

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
