#pragma once

// Where the running executable lives. Installed builds keep their data next
// to the executables (templates/, plugins/, sdk/, docs/), so lookups that
// also know a build-tree path try <exe dir>/... first.

#include <string>

namespace ts {
namespace tombstone {
namespace core {

// Folder of the running executable (no trailing separator), or "" if unknown.
std::string executable_dir();

}  // namespace core
}  // namespace tombstone
}  // namespace ts
