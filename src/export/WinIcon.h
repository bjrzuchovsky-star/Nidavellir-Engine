#pragma once

// Windows: stamps a PNG as the exe's icon (RT_ICON + RT_GROUP_ICON through
// UpdateResource; PNG icon entries are valid on Vista+). Elsewhere a no-op
// that returns false with a reason.
#include <string>

namespace ts {
namespace tombstone {
namespace exporter {

bool stamp_exe_icon(const std::string& exe, const std::string& png, std::string* error);

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
