#pragma once

// Integrity for Trading Post downloads: SHA-256 (package hashes pinned in
// the index) and Ed25519 (the index itself is signed by a publisher key the
// user trusts). Ed25519 comes from Monocypher (fetched by hash).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ts {
namespace tombstone {
namespace store {

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);
std::string sha256_hex(std::string_view bytes);
bool sha256_file_hex(const std::string& path, std::string* hex, std::string* error = nullptr);

std::string to_hex(const std::uint8_t* data, std::size_t size);
// Whitespace is ignored; false on odd length / non-hex.
bool from_hex(std::string_view hex, std::vector<std::uint8_t>* out);

// Ed25519 (RFC 8032). Keys / signatures as lowercase hex strings.
struct KeyPair {
  std::string public_hex;  // 32 bytes
  std::string secret_hex;  // 64 bytes (seed + public key, Monocypher layout)
};
// Deterministic from a 32-byte seed (hex). False on a bad seed.
bool ed25519_keypair_from_seed(const std::string& seed_hex, KeyPair* out);
std::string ed25519_sign(const std::string& secret_hex, std::string_view message);
bool ed25519_verify(const std::string& public_hex, const std::string& signature_hex,
                    std::string_view message);

}  // namespace store
}  // namespace tombstone
}  // namespace ts
