#pragma once

// Town Hall security bits (ts_mmo): OS randomness, hex, salted password
// hashes and session tokens. Passwords are hashed with Argon2i (Monocypher
// crypto_argon2, RFC 9106) over a fresh 16-byte random salt, and stored as
//
//   argon2i$m=<KiB>$t=<passes>$<salt hex>$<hash hex>
//
// so the cost can rise later without breaking old accounts. Checks compare
// in constant time (crypto_verify32). Session tokens are 32 random bytes;
// the database only keeps their BLAKE2b hash.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mmo {

// Cryptographically secure bytes from the OS (BCryptGenRandom / getrandom /
// /dev/urandom). False when the OS refused.
bool random_bytes(std::uint8_t* out, std::size_t n);
std::string to_hex(const std::uint8_t* data, std::size_t n);
bool from_hex(const std::string& hex, std::vector<std::uint8_t>* out);

struct PasswordCost {
  std::uint32_t kib = 4096;  // Argon2 memory (KiB)
  std::uint32_t passes = 3;
};

// "" when the OS gave no randomness.
std::string hash_password(const std::string& password, const PasswordCost& cost = {});
bool verify_password(const std::string& password, const std::string& stored);

// 64 hex chars of fresh randomness.
std::string new_token();
// BLAKE2b-256 of the token, hex (what the sessions table stores).
std::string token_digest(const std::string& token);

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
