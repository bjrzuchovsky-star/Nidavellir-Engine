#pragma once

// SHA-256 (FIPS 180-4) for The Vault's content addressing. Self-contained
// so the headless ts_vault server needs no crypto library.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ts {
namespace tombstone {
namespace vault {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t size);
  void update(std::string_view s) { update(s.data(), s.size()); }
  // Lowercase hex digest; the object is spent afterwards.
  std::string hex();

 private:
  void block(const std::uint8_t* p);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t used_ = 0;
  std::uint64_t total_ = 0;
};

std::string sha256_hex(std::string_view bytes);
// "" when the file cannot be read; size_out receives the byte count.
std::string sha256_file(const std::string& path, std::uint64_t* size_out = nullptr);
bool is_sha256_hex(std::string_view s);

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
