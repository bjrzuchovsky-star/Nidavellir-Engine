#include "export/Zip.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace ts {
namespace tombstone {
namespace exporter {

namespace fs = std::filesystem;

std::uint32_t crc32(const void* data, std::size_t size) {
  static std::uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  std::uint32_t c = 0xFFFFFFFFu;
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

namespace {
void u16(std::string& s, std::uint32_t v) {
  s.push_back(static_cast<char>(v & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void u32(std::string& s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
}  // namespace

bool zip_folder(const std::string& dir, const std::string& root_name,
                const std::string& zip_path, std::string* error) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec)) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  std::ofstream out(fs::path(zip_path), std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + zip_path;
    return false;
  }
  std::string central;
  std::uint32_t offset = 0;
  std::uint16_t count = 0;
  for (const fs::path& f : files) {
    std::ifstream in(f, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    const std::string name = root_name + "/" + fs::relative(f, dir, ec).generic_string();
    const std::uint32_t crc = crc32(data.data(), data.size());
    const auto size = static_cast<std::uint32_t>(data.size());
    // Unix exec bit for the launcher + binaries (external attrs, made-by unix).
    const bool exec = f.extension() == ".sh" || f.extension().empty();
    std::string local;
    u32(local, 0x04034b50);
    u16(local, 20); u16(local, 0x0800); u16(local, 0);  // version, utf-8, stored
    u16(local, 0); u16(local, 0x21);                     // time, date (1980-01-01)
    u32(local, crc); u32(local, size); u32(local, size);
    u16(local, static_cast<std::uint32_t>(name.size())); u16(local, 0);
    local += name;
    out.write(local.data(), static_cast<std::streamsize>(local.size()));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    u32(central, 0x02014b50);
    u16(central, (3u << 8) | 20); u16(central, 20); u16(central, 0x0800); u16(central, 0);
    u16(central, 0); u16(central, 0x21);
    u32(central, crc); u32(central, size); u32(central, size);
    u16(central, static_cast<std::uint32_t>(name.size()));
    u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
    u32(central, (exec ? 0100755u : 0100644u) << 16);
    u32(central, offset);
    central += name;
    offset += static_cast<std::uint32_t>(local.size() + data.size());
    ++count;
  }
  std::string end;
  u32(end, 0x06054b50);
  u16(end, 0); u16(end, 0); u16(end, count); u16(end, count);
  u32(end, static_cast<std::uint32_t>(central.size())); u32(end, offset); u16(end, 0);
  out.write(central.data(), static_cast<std::streamsize>(central.size()));
  out.write(end.data(), static_cast<std::streamsize>(end.size()));
  if (!out) {
    if (error) *error = "short write to " + zip_path;
    return false;
  }
  return true;
}

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
