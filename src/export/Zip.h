#pragma once

// Minimal zip writer (stored entries, CRC-32, no zip64): enough for a game
// folder well under 4 GB, opens in Explorer / unzip / 7-Zip.
#include <cstddef>
#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace exporter {

std::uint32_t crc32(const void* data, std::size_t size);
// Zips every file under dir, entries named <root_name>/<relative path>.
bool zip_folder(const std::string& dir, const std::string& root_name,
                const std::string& zip_path, std::string* error);

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
