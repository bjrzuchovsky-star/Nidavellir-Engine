#pragma once

// Native "pick files" dialog for the Vault manager's Add files...
// Windows: the common Open dialog (multi-select). Elsewhere there is no
// native picker (available() is false): paste paths or drop files instead.

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace filepick {

bool available();
// Absolute UTF-8 paths; empty when cancelled. Blocks until the dialog closes.
std::vector<std::string> open_files(const std::string& title);

}  // namespace filepick
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
