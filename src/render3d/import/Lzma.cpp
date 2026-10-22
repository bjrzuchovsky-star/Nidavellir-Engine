// LZMA decoder, clean-room from the public LZMA algorithm description (see
// Lzma.h). The whole output buffer doubles as the dictionary.
#include "render3d/import/Lzma.h"

#include <algorithm>
#include <cstring>

namespace ts {
namespace tombstone {
namespace render3d {
namespace lzma {

namespace {

constexpr int kProbBits = 11;
constexpr std::uint16_t kProbInit = 1 << (kProbBits - 1);
constexpr int kMoveBits = 5;
constexpr std::uint32_t kTop = 1u << 24;
constexpr int kStates = 12;
constexpr int kPosBitsMax = 4;
constexpr int kEndPosModel = 14;
constexpr int kFullDistances = 128;
constexpr int kAlignBits = 4;

struct Range {
  const unsigned char* p;
  const unsigned char* end;
  std::uint32_t range = 0xFFFFFFFFu, code = 0;
  bool bad = false;
  unsigned char byte() {
    if (p < end) return *p++;
    bad = true;
    return 0;
  }
  void init() {
    if (byte() != 0) bad = true;
    for (int i = 0; i < 4; ++i) code = (code << 8) | byte();
    if (code == range) bad = true;
  }
  void norm() {
    if (range < kTop) {
      range <<= 8;
      code = (code << 8) | byte();
    }
  }
  std::uint32_t direct(int n) {
    std::uint32_t r = 0;
    while (n-- > 0) {
      range >>= 1;
      code -= range;
      const std::uint32_t t = 0u - (code >> 31);
      code += range & t;
      if (code == range) bad = true;
      norm();
      r = (r << 1) + (t + 1);
    }
    return r;
  }
  int bit(std::uint16_t* prob) {
    std::uint32_t v = *prob;
    const std::uint32_t bound = (range >> kProbBits) * v;
    int s;
    if (code < bound) {
      v += ((1u << kProbBits) - v) >> kMoveBits;
      range = bound;
      s = 0;
    } else {
      v -= v >> kMoveBits;
      code -= bound;
      range -= bound;
      s = 1;
    }
    *prob = static_cast<std::uint16_t>(v);
    norm();
    return s;
  }
  std::uint32_t tree(std::uint16_t* probs, int bits) {
    std::uint32_t m = 1;
    for (int i = 0; i < bits; ++i) m = (m << 1) + bit(&probs[m]);
    return m - (1u << bits);
  }
  std::uint32_t tree_reverse(std::uint16_t* probs, int bits) {
    std::uint32_t m = 1, sym = 0;
    for (int i = 0; i < bits; ++i) {
      const std::uint32_t b = bit(&probs[m]);
      m = (m << 1) + b;
      sym |= b << i;
    }
    return sym;
  }
};

struct LenCoder {
  std::uint16_t choice = kProbInit, choice2 = kProbInit;
  std::uint16_t low[1 << kPosBitsMax][1 << 3];
  std::uint16_t mid[1 << kPosBitsMax][1 << 3];
  std::uint16_t high[1 << 8];
  LenCoder() {
    std::fill(&low[0][0], &low[0][0] + sizeof(low) / 2, kProbInit);
    std::fill(&mid[0][0], &mid[0][0] + sizeof(mid) / 2, kProbInit);
    std::fill(high, high + 256, kProbInit);
  }
  std::uint32_t decode(Range& rc, unsigned pos_state) {
    if (!rc.bit(&choice)) return rc.tree(low[pos_state], 3);
    if (!rc.bit(&choice2)) return 8 + rc.tree(mid[pos_state], 3);
    return 16 + rc.tree(high, 8);
  }
};

}  // namespace

bool looks_like_alone(const unsigned char* d, std::size_t n) {
  if (n < 13 + 5) return false;
  if (std::memcmp(d, "GMAD", 4) == 0) return false;
  if (d[0] >= 9 * 5 * 5) return false;
  std::uint32_t dict = 0;
  for (int i = 0; i < 4; ++i) dict |= std::uint32_t(d[1 + i]) << (8 * i);
  if (dict < (1u << 12) && dict != 0) return false;
  std::uint64_t size = 0;
  for (int i = 0; i < 8; ++i) size |= std::uint64_t(d[5 + i]) << (8 * i);
  if (size != ~0ull && size > (64ull << 30)) return false;
  return d[13] == 0;  // range coder's first byte is always 0
}

bool decode_alone(const unsigned char* d, std::size_t n, std::vector<unsigned char>* out_ptr,
                  std::string* error, std::uint64_t max_out) {
  auto fail = [&](const char* why) {
    if (error) *error = std::string("LZMA: ") + why;
    return false;
  };
  if (n < 13) return fail("truncated header");
  unsigned props = d[0];
  if (props >= 9 * 5 * 5) return fail("bad properties byte");
  const int lc = static_cast<int>(props % 9);
  props /= 9;
  const int lp = static_cast<int>(props % 5);
  const int pb = static_cast<int>(props / 5);
  std::uint64_t size = 0;
  for (int i = 0; i < 8; ++i) size |= std::uint64_t(d[5 + i]) << (8 * i);
  const bool known = size != ~0ull;
  if (known && size > max_out) return fail("uncompressed size too large");
  std::vector<unsigned char>& out = *out_ptr;
  out.clear();
  if (known) out.reserve(static_cast<std::size_t>(size));

  std::vector<std::uint16_t> lit(std::size_t(0x300) << (lc + lp), kProbInit);
  std::uint16_t is_match[kStates << kPosBitsMax], is_rep[kStates], is_rep_g0[kStates], is_rep_g1[kStates],
      is_rep_g2[kStates], is_rep0_long[kStates << kPosBitsMax];
  std::uint16_t pos_slot[4][1 << 6], pos_dec[1 + kFullDistances - kEndPosModel], align[1 << kAlignBits];
  for (auto* a : {is_match, is_rep0_long}) std::fill(a, a + (kStates << kPosBitsMax), kProbInit);
  for (auto* a : {is_rep, is_rep_g0, is_rep_g1, is_rep_g2}) std::fill(a, a + kStates, kProbInit);
  std::fill(&pos_slot[0][0], &pos_slot[0][0] + 4 * 64, kProbInit);
  std::fill(pos_dec, pos_dec + sizeof(pos_dec) / 2, kProbInit);
  std::fill(align, align + (1 << kAlignBits), kProbInit);
  LenCoder len_dec, rep_len_dec;

  Range rc{d + 13, d + n};
  rc.init();
  if (rc.bad) return fail("bad range coder start");
  std::uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
  unsigned state = 0;
  const unsigned pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
  for (;;) {
    if (known && out.size() >= size) break;
    if (rc.bad) return fail("unexpected end of data");
    if (!known && out.size() >= max_out) return fail("output too large");
    const unsigned pos_state = static_cast<unsigned>(out.size()) & pb_mask;
    if (!rc.bit(&is_match[(state << kPosBitsMax) + pos_state])) {
      const unsigned prev = out.empty() ? 0 : out.back();
      const std::size_t ls = ((static_cast<unsigned>(out.size()) & lp_mask) << lc) + (prev >> (8 - lc));
      std::uint16_t* probs = &lit[0x300 * ls];
      unsigned sym = 1;
      if (state >= 7) {
        unsigned match = out[out.size() - rep0 - 1];
        do {
          const unsigned mb = (match >> 7) & 1;
          match <<= 1;
          const unsigned b = rc.bit(&probs[((1 + mb) << 8) + sym]);
          sym = (sym << 1) | b;
          if (mb != b) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) sym = (sym << 1) | rc.bit(&probs[sym]);
      out.push_back(static_cast<unsigned char>(sym - 0x100));
      state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
      continue;
    }
    std::uint32_t len;
    if (rc.bit(&is_rep[state])) {
      if (out.empty()) return fail("repeat before any data");
      if (!rc.bit(&is_rep_g0[state])) {
        if (!rc.bit(&is_rep0_long[(state << kPosBitsMax) + pos_state])) {
          state = state < 7 ? 9 : 11;
          out.push_back(out[out.size() - rep0 - 1]);
          continue;
        }
      } else {
        std::uint32_t dist;
        if (!rc.bit(&is_rep_g1[state])) {
          dist = rep1;
        } else {
          if (!rc.bit(&is_rep_g2[state])) {
            dist = rep2;
          } else {
            dist = rep3;
            rep3 = rep2;
          }
          rep2 = rep1;
        }
        rep1 = rep0;
        rep0 = dist;
      }
      len = rep_len_dec.decode(rc, pos_state);
      state = state < 7 ? 8 : 11;
    } else {
      rep3 = rep2;
      rep2 = rep1;
      rep1 = rep0;
      len = len_dec.decode(rc, pos_state);
      state = state < 7 ? 7 : 10;
      const unsigned ls = std::min<std::uint32_t>(len, 3);
      const std::uint32_t slot = rc.tree(pos_slot[ls], 6);
      std::uint32_t dist;
      if (slot < 4) {
        dist = slot;
      } else {
        const int direct = static_cast<int>((slot >> 1) - 1);
        dist = (2 | (slot & 1)) << direct;
        if (slot < kEndPosModel) {
          dist += rc.tree_reverse(pos_dec + dist - slot, direct);
        } else {
          dist += rc.direct(direct - kAlignBits) << kAlignBits;
          dist += rc.tree_reverse(align, kAlignBits);
        }
      }
      rep0 = dist;
      if (rep0 == 0xFFFFFFFFu) break;  // end marker
      if (rep0 >= out.size()) return fail("match distance outside the data");
    }
    len += 2;
    if (known && out.size() + len > size) return fail("match runs past the stated size");
    const std::size_t from = out.size() - rep0 - 1;
    for (std::uint32_t i = 0; i < len; ++i) out.push_back(out[from + i]);
  }
  if (rc.bad) return fail("truncated data");
  return true;
}

std::vector<unsigned char> encode_alone_literals(const unsigned char* data, std::size_t size) {
  const int lc = 3, pb = 2;
  std::vector<unsigned char> out;
  out.push_back(static_cast<unsigned char>((pb * 5 + 0) * 9 + lc));
  const std::uint32_t dict = 1u << 16;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(dict >> (8 * i)));
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<unsigned char>(std::uint64_t(size) >> (8 * i)));
  std::uint64_t low = 0;
  std::uint32_t range = 0xFFFFFFFFu;
  unsigned char cache = 0;
  std::uint64_t cache_size = 1;
  auto shift_low = [&]() {
    if (static_cast<std::uint32_t>(low) < 0xFF000000u || (low >> 32) != 0) {
      unsigned char temp = cache;
      do {
        out.push_back(static_cast<unsigned char>(temp + static_cast<unsigned char>(low >> 32)));
        temp = 0xFF;
      } while (--cache_size != 0);
      cache = static_cast<unsigned char>(low >> 24);
    }
    ++cache_size;
    low = (low & 0x00FFFFFFu) << 8;
  };
  auto bit = [&](std::uint16_t* p, unsigned b) {
    const std::uint32_t bound = (range >> kProbBits) * *p;
    if (!b) {
      range = bound;
      *p = static_cast<std::uint16_t>(*p + (((1u << kProbBits) - *p) >> kMoveBits));
    } else {
      low += bound;
      range -= bound;
      *p = static_cast<std::uint16_t>(*p - (*p >> kMoveBits));
    }
    while (range < kTop) {
      range <<= 8;
      shift_low();
    }
  };
  std::vector<std::uint16_t> lit(std::size_t(0x300) << lc, kProbInit);
  std::uint16_t is_match[kStates << kPosBitsMax];
  std::fill(is_match, is_match + (kStates << kPosBitsMax), kProbInit);
  unsigned prev = 0;
  for (std::size_t i = 0; i < size; ++i) {
    bit(&is_match[(0 << kPosBitsMax) + (i & 3)], 0);
    std::uint16_t* probs = &lit[0x300 * (prev >> (8 - lc))];
    unsigned ctx = 1;
    for (int k = 7; k >= 0; --k) {
      const unsigned b = (data[i] >> k) & 1;
      bit(&probs[ctx], b);
      ctx = (ctx << 1) | b;
    }
    prev = data[i];
  }
  for (int i = 0; i < 5; ++i) shift_low();
  return out;
}

}  // namespace lzma
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
