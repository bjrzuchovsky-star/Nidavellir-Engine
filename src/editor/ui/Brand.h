#pragma once

#include "core/Brand.h"

#include <cstddef>

namespace ts {
namespace tombstone {
namespace editor {
namespace brand {

// Aliases of src/core/Brand.h so every screen shares one product name.
inline constexpr const char* kProduct = ts::tombstone::brand::kProduct;
inline constexpr const char* kAdminTitle = ts::tombstone::brand::kProduct;
inline constexpr const char* kTagline = ts::tombstone::brand::kTagline;
inline constexpr const char* kWindowTitle = ts::tombstone::brand::kWindowTitle;
inline constexpr const char* kWindowTitlePrefix = ts::tombstone::brand::kWindowTitlePrefix;

// Loading splash status lines, picked by progress.
inline const char* LoadingLine(float progress) {
  static constexpr const char* kLines[] = {
      "Stoking the forge",
      "Laying the anvil",
      "Reading the runes",
      "Quenching the steel",
      "Opening the hall",
  };
  constexpr std::size_t kCount = sizeof(kLines) / sizeof(kLines[0]);
  if (progress < 0.0f) {
    progress = 0.0f;
  }
  std::size_t idx = static_cast<std::size_t>(progress * kCount);
  if (idx >= kCount) {
    idx = kCount - 1;
  }
  return kLines[idx];
}

}  // namespace brand
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
