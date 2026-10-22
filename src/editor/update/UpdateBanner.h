#pragma once

#include <string>

namespace ts {
namespace tombstone {
namespace editor {

// "New update available" notice (AppFlow fills it; see docs/updates.md).
struct UpdateBanner {
  bool visible = false;
  std::string version;        // "0.2.0"
  std::string current;        // the running version
  bool busy = false;          // downloading / verifying
  double progress = -1.0;     // 0..1 while busy, -1 unknown
  std::string note;           // "Fetching TombStone-0.2.0-update.exe..."
  std::string error;          // last Update failure (shown in the notice)
  bool fake = false;          // TS_FAKE_UPDATE test banner
};
enum class UpdateAction { None, Update, ReleaseNotes, Later, Skip };

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
