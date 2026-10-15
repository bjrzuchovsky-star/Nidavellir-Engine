#include "store/Crypto.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
#include "monocypher.h"
#include "monocypher-ed25519.h"
}

namespace ts {
namespace tombstone {
namespace store {

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint8_t buf[64] = {};
  std::size_t used = 0;
  std::uint64_t total = 0;

  void block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
             (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const std::uint8_t* p, std::size_t n) {
    total += n;
    while (n > 0) {
      const std::size_t take = std::min<std::size_t>(64 - used, n);
      std::memcpy(buf + used, p, take);
      used += take;
      p += take;
      n -= take;
      if (used == 64) {
        block(buf);
        used = 0;
      }
    }
  }
  std::array<std::uint8_t, 32> finish() {
    const std::uint64_t bits = total * 8;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    const std::uint8_t zero = 0;
    while (used != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
      out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
      out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
      out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
  }
};

}  // namespace

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size) {
  Sha256 s;
  s.update(static_cast<const std::uint8_t*>(data), size);
  return s.finish();
}

std::string sha256_hex(std::string_view bytes) {
  const auto d = sha256(bytes.data(), bytes.size());
  return to_hex(d.data(), d.size());
}

bool sha256_file_hex(const std::string& path, std::string* hex, std::string* error) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "could not open " + path;
    return false;
  }
  Sha256 s;
  char buf[65536];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize n = in.gcount();
    if (n > 0) s.update(reinterpret_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(n));
  }
  const auto d = s.finish();
  *hex = to_hex(d.data(), d.size());
  return true;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

bool from_hex(std::string_view hex, std::vector<std::uint8_t>* out) {
  out->clear();
  int hi = -1;
  for (char c : hex) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    else return false;
    if (hi < 0) {
      hi = v;
    } else {
      out->push_back(static_cast<std::uint8_t>(hi * 16 + v));
      hi = -1;
    }
  }
  return hi < 0;
}

bool ed25519_keypair_from_seed(const std::string& seed_hex, KeyPair* out) {
  std::vector<std::uint8_t> seed;
  if (!from_hex(seed_hex, &seed) || seed.size() != 32) return false;
  std::uint8_t sk[64], pk[32];
  crypto_ed25519_key_pair(sk, pk, seed.data());  // wipes seed
  out->public_hex = to_hex(pk, 32);
  out->secret_hex = to_hex(sk, 64);
  crypto_wipe(sk, sizeof(sk));
  return true;
}

std::string ed25519_sign(const std::string& secret_hex, std::string_view message) {
  std::vector<std::uint8_t> sk;
  if (!from_hex(secret_hex, &sk) || sk.size() != 64) return {};
  std::uint8_t sig[64];
  crypto_ed25519_sign(sig, sk.data(), reinterpret_cast<const std::uint8_t*>(message.data()),
                      message.size());
  crypto_wipe(sk.data(), sk.size());
  return to_hex(sig, 64);
}

bool ed25519_verify(const std::string& public_hex, const std::string& signature_hex,
                    std::string_view message) {
  std::vector<std::uint8_t> pk, sig;
  if (!from_hex(public_hex, &pk) || pk.size() != 32) return false;
  if (!from_hex(signature_hex, &sig) || sig.size() != 64) return false;
  return crypto_ed25519_check(sig.data(), pk.data(),
                              reinterpret_cast<const std::uint8_t*>(message.data()),
                              message.size()) == 0;
}

}  // namespace store
}  // namespace tombstone
}  // namespace ts
