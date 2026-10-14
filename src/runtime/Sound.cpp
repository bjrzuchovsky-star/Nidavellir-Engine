#include "runtime/Sound.h"

#include <cstring>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

constexpr std::uint8_t kWireVersion = 1;
constexpr std::size_t kMaxText = 512;
constexpr std::size_t kMaxList = 64;

struct Writer {
  std::vector<std::uint8_t> b;
  void u8(std::uint8_t v) { b.push_back(v); }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void f32(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    u32(v);
  }
  void str(const std::string& s) {
    const std::size_t n = s.size() < kMaxText ? s.size() : kMaxText;
    u32(static_cast<std::uint32_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
  }
};

struct Reader {
  const std::vector<std::uint8_t>& b;
  std::size_t at = 0;
  bool ok = true;
  bool need(std::size_t n) {
    if (!ok || at + n > b.size()) ok = false;
    return ok;
  }
  std::uint8_t u8() { return need(1) ? b[at++] : 0; }
  std::uint32_t u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at++]) << (8 * i);
    return v;
  }
  std::uint64_t u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[at++]) << (8 * i);
    return v;
  }
  float f32() {
    const std::uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f == f ? f : 0.0f;  // no NaN off the wire
  }
  std::string str() {
    const std::uint32_t n = u32();
    if (n > kMaxText || !need(n)) {
      ok = false;
      return {};
    }
    std::string s(b.begin() + static_cast<std::ptrdiff_t>(at),
                  b.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
    return s;
  }
};

}  // namespace

std::vector<std::uint8_t> encode_sound(const SoundEvent& ev) {
  Writer w;
  w.u8(kWireVersion);
  w.u8(static_cast<std::uint8_t>(ev.kind));
  w.str(ev.clip);
  w.str(ev.bus);
  w.f32(ev.volume);
  w.f32(ev.pitch);
  w.f32(ev.variation);
  w.u8(static_cast<std::uint8_t>((ev.positional ? 1 : 0) | (ev.loop ? 2 : 0) |
                                 (ev.three_d ? 4 : 0)));
  w.f32(ev.x);
  w.f32(ev.y);
  w.f32(ev.z);
  w.f32(ev.fade);
  w.u64(ev.entity);
  const std::size_t n = ev.list.size() < kMaxList ? ev.list.size() : kMaxList;
  w.u32(static_cast<std::uint32_t>(n));
  for (std::size_t i = 0; i < n; ++i) w.str(ev.list[i]);
  return w.b;
}

bool decode_sound(const std::vector<std::uint8_t>& bytes, SoundEvent* out) {
  Reader r{bytes};
  if (r.u8() != kWireVersion) return false;
  SoundEvent ev;
  const std::uint8_t kind = r.u8();
  if (kind > static_cast<std::uint8_t>(SoundEvent::Kind::Stop)) return false;
  ev.kind = static_cast<SoundEvent::Kind>(kind);
  ev.clip = r.str();
  ev.bus = r.str();
  ev.volume = r.f32();
  ev.pitch = r.f32();
  ev.variation = r.f32();
  const std::uint8_t flags = r.u8();
  ev.positional = (flags & 1) != 0;
  ev.loop = (flags & 2) != 0;
  ev.three_d = (flags & 4) != 0;
  ev.x = r.f32();
  ev.y = r.f32();
  ev.z = r.f32();
  ev.fade = r.f32();
  ev.entity = r.u64();
  const std::uint32_t n = r.u32();
  if (n > kMaxList) return false;
  for (std::uint32_t i = 0; i < n && r.ok; ++i) ev.list.push_back(r.str());
  if (!r.ok || r.at != bytes.size()) return false;
  if (out) *out = std::move(ev);
  return true;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
