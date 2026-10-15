#include "mmo/MmoCrypto.h"

#include "monocypher.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace ts {
namespace tombstone {
namespace mmo {

bool random_bytes(std::uint8_t* out, std::size_t n) {
  if (n == 0) return true;
#if defined(_WIN32)
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  std::size_t got = 0;
#if defined(__linux__)
  while (got < n) {
    const ssize_t r = getrandom(out + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    got += static_cast<std::size_t>(r);
  }
  if (got == n) return true;
#endif
  const int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd < 0) return false;
  while (got < n) {
    const ssize_t r = ::read(fd, out + got, n - got);
    if (r <= 0) {
      if (r < 0 && errno == EINTR) continue;
      break;
    }
    got += static_cast<std::size_t>(r);
  }
  ::close(fd);
  return got == n;
#endif
}

std::string to_hex(const std::uint8_t* data, std::size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(d[data[i] >> 4]);
    s.push_back(d[data[i] & 15]);
  }
  return s;
}

bool from_hex(const std::string& hex, std::vector<std::uint8_t>* out) {
  if (hex.size() % 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out->clear();
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int a = nib(hex[i]), b = nib(hex[i + 1]);
    if (a < 0 || b < 0) return false;
    out->push_back(static_cast<std::uint8_t>(a * 16 + b));
  }
  return true;
}

namespace {

constexpr std::size_t kSalt = 16;
constexpr std::size_t kHash = 32;

bool argon2(const std::string& password, const std::uint8_t* salt, std::size_t salt_n,
            std::uint32_t kib, std::uint32_t passes, std::uint8_t out[kHash]) {
  if (kib < 8 || kib > 1024u * 1024u || passes < 1 || passes > 16) return false;
  void* work = std::malloc(static_cast<std::size_t>(kib) * 1024u);
  if (!work) return false;
  crypto_argon2_config cfg{};
  cfg.algorithm = CRYPTO_ARGON2_I;
  cfg.nb_blocks = kib;
  cfg.nb_passes = passes;
  cfg.nb_lanes = 1;
  crypto_argon2_inputs in{};
  in.pass = reinterpret_cast<const std::uint8_t*>(password.data());
  in.pass_size = static_cast<std::uint32_t>(password.size());
  in.salt = salt;
  in.salt_size = static_cast<std::uint32_t>(salt_n);
  crypto_argon2(out, kHash, work, cfg, in, crypto_argon2_no_extras);
  crypto_wipe(work, static_cast<std::size_t>(kib) * 1024u);
  std::free(work);
  return true;
}

}  // namespace

std::string hash_password(const std::string& password, const PasswordCost& cost) {
  std::uint8_t salt[kSalt];
  if (!random_bytes(salt, kSalt)) return {};
  std::uint8_t h[kHash];
  if (!argon2(password, salt, kSalt, cost.kib, cost.passes, h)) return {};
  std::ostringstream o;
  o << "argon2i$m=" << cost.kib << "$t=" << cost.passes << "$" << to_hex(salt, kSalt) << "$"
    << to_hex(h, kHash);
  crypto_wipe(h, kHash);
  return o.str();
}

bool verify_password(const std::string& password, const std::string& stored) {
  // argon2i$m=K$t=T$salt$hash
  std::vector<std::string> parts;
  std::string cur;
  for (char c : stored) {
    if (c == '$') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  if (parts.size() != 5 || parts[0] != "argon2i" || parts[1].rfind("m=", 0) != 0 ||
      parts[2].rfind("t=", 0) != 0)
    return false;
  const unsigned long kib = std::strtoul(parts[1].c_str() + 2, nullptr, 10);
  const unsigned long passes = std::strtoul(parts[2].c_str() + 2, nullptr, 10);
  std::vector<std::uint8_t> salt, want;
  if (!from_hex(parts[3], &salt) || !from_hex(parts[4], &want) || want.size() != kHash || salt.size() < 8)
    return false;
  std::uint8_t got[kHash];
  if (!argon2(password, salt.data(), salt.size(), static_cast<std::uint32_t>(kib),
              static_cast<std::uint32_t>(passes), got))
    return false;
  const bool ok = crypto_verify32(got, want.data()) == 0;
  crypto_wipe(got, kHash);
  return ok;
}

std::string new_token() {
  std::uint8_t b[32];
  if (!random_bytes(b, sizeof b)) return {};
  std::string t = to_hex(b, sizeof b);
  crypto_wipe(b, sizeof b);
  return t;
}

std::string token_digest(const std::string& token) {
  std::uint8_t h[32];
  crypto_blake2b(h, sizeof h, reinterpret_cast<const std::uint8_t*>(token.data()), token.size());
  return to_hex(h, sizeof h);
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
