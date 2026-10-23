#pragma once

// LZMA "alone" (.lzma) decoder. Written from the public LZMA algorithm
// description (the LZMA SDK's lzma-specification, which is in the public
// domain); no third-party code. Used for compressed Source engine addon
// downloads (.gma that arrive LZMA-compressed, often named .bin).
//
// Stream layout: 1 properties byte (lc / lp / pb), u32 dictionary size,
// u64 uncompressed size (all 0xFF = unknown, then an end marker ends the
// data), then the range-coded data.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace lzma {

// Cheap header check: plausible properties byte, dictionary size and
// uncompressed size. Never true for data starting with "GMAD".
bool looks_like_alone(const unsigned char* data, std::size_t size);

// Decodes a whole .lzma stream. `max_out` caps the output (corrupt sizes).
bool decode_alone(const unsigned char* data, std::size_t size, std::vector<unsigned char>* out,
                  std::string* error, std::uint64_t max_out = 4ull << 30);

// Minimal encoder for tests: every byte coded as a literal (valid LZMA,
// no compression), known size. Lets smokes build compressed fixtures.
std::vector<unsigned char> encode_alone_literals(const unsigned char* data, std::size_t size);

}  // namespace lzma
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
