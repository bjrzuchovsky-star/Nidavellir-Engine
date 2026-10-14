#include "mp/Protocol.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace mp {

using net::ByteReader;
using net::ByteWriter;

namespace {

constexpr std::uint32_t kHelloMagic = 0x504D5354u;  // "TSMP"

ByteWriter start(MsgType t) {
  ByteWriter w;
  w.u8(static_cast<std::uint8_t>(t));
  return w;
}

bool open(const std::vector<std::uint8_t>& data, MsgType want, ByteReader* r) {
  if (data.empty() || data[0] != static_cast<std::uint8_t>(want)) return false;
  r->u8();
  return true;
}

std::int8_t q8(float v) {
  const float c = std::clamp(v, -1.0f, 1.0f);
  return static_cast<std::int8_t>(std::lround(c * 127.0f));
}
float uq8(std::uint8_t b) {
  return static_cast<float>(static_cast<std::int8_t>(b)) / 127.0f;
}

bool finite(float v) { return std::isfinite(v); }

enum RowFlags : std::uint8_t {
  kRowHidden = 1u << 0,
  kRowCollider = 1u << 1,
  kRowRider = 1u << 2,
  kRowSpawned = 1u << 3,
  kRowFaceLeft = 1u << 4,
};

}  // namespace

const char* reason_text(std::uint32_t reason) {
  switch (reason) {
    case kReasonFull: return "the posse is full (four riders)";
    case kReasonShutdown: return "the server closed up shop";
    case kReasonVersion: return "protocol version mismatch";
    case kReasonQuit: return "rode off";
    case kReasonBadMessage: return "malformed message";
    case kReasonKicked: return "run out of town";
    default: return "lost the trail (timed out)";
  }
}

std::string clean_name(const std::string& name) {
  std::string out;
  for (char c : name) {
    if (c >= 32 && c < 127) out.push_back(c);
    if (out.size() >= kMaxNameLength) break;
  }
  const auto b = out.find_first_not_of(' ');
  if (b == std::string::npos) return "Stranger";
  const auto e = out.find_last_not_of(' ');
  return out.substr(b, e - b + 1);
}

std::uint64_t scene_hash(const std::string& scene_json) {
  return net::fnv1a64(scene_json.data(), scene_json.size());
}

bool peek_type(const std::vector<std::uint8_t>& data, MsgType* out) {
  if (data.empty() || data[0] < 1 || data[0] > 7) return false;
  *out = static_cast<MsgType>(data[0]);
  return true;
}

// --- Hello ------------------------------------------------------------------

std::vector<std::uint8_t> encode(const HelloMsg& m) {
  ByteWriter w = start(MsgType::Hello);
  w.u32(kHelloMagic);
  w.u16(m.version);
  w.str(m.name, kMaxNameLength * 4);
  w.u64(m.scene_hash);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, HelloMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Hello, &r)) return false;
  if (r.u32() != kHelloMagic) return false;
  out->version = r.u16();
  out->name = r.str(kMaxNameLength * 4);
  out->scene_hash = r.u64();
  return r.ok();
}

// --- Welcome / Reject -------------------------------------------------------

std::vector<std::uint8_t> encode(const WelcomeMsg& m) {
  ByteWriter w = start(MsgType::Welcome);
  w.u8(static_cast<std::uint8_t>(m.slot));
  w.varu(m.rider);
  w.varu(m.tick);
  w.u8(static_cast<std::uint8_t>(m.tick_rate));
  w.u8(static_cast<std::uint8_t>(m.snapshot_every));
  w.str(m.project_name, 256);
  w.str(m.scene_json, kMaxSceneBytes);
  w.u64(m.scene_hash);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, WelcomeMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Welcome, &r)) return false;
  out->slot = r.u8();
  out->rider = r.varu();
  out->tick = r.varu();
  out->tick_rate = r.u8();
  out->snapshot_every = r.u8();
  out->project_name = r.str(256);
  out->scene_json = r.str(kMaxSceneBytes);
  out->scene_hash = r.u64();
  return r.ok() && out->slot < kMaxPosse && out->tick_rate > 0 &&
         out->snapshot_every > 0;
}

std::vector<std::uint8_t> encode(const RejectMsg& m) {
  ByteWriter w = start(MsgType::Reject);
  w.u32(m.reason);
  w.str(m.text, 512);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, RejectMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Reject, &r)) return false;
  out->reason = r.u32();
  out->text = r.str(512);
  return r.ok();
}

// --- Input ------------------------------------------------------------------

std::vector<std::uint8_t> encode(const InputMsg& m) {
  ByteWriter w = start(MsgType::Input);
  w.u32(m.client_time_ms);
  w.u16(m.rtt_ms);
  const std::size_t n = std::min<std::size_t>(m.cmds.size(), 32);
  w.u8(static_cast<std::uint8_t>(n));
  if (n > 0) w.varu(m.cmds[0].seq);
  for (std::size_t i = 0; i < n; ++i) {
    const runtime::PlayerInput& in = m.cmds[i].input;
    w.u8(static_cast<std::uint8_t>(q8(in.move_x)));
    w.u8(static_cast<std::uint8_t>(q8(in.move_y)));
    w.u8(static_cast<std::uint8_t>(in.buttons & 0xFFu));
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, InputMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Input, &r)) return false;
  out->client_time_ms = r.u32();
  out->rtt_ms = r.u16();
  const std::size_t n = r.u8();
  out->cmds.clear();
  if (n > 32) return false;
  std::uint64_t seq = n > 0 ? r.varu() : 0;
  if (seq > 0xFFFFFFFFull || seq < n) return n == 0 && r.ok();
  for (std::size_t i = 0; i < n; ++i) {
    InputCmd c;
    c.seq = static_cast<std::uint32_t>(seq - i);
    c.input.move_x = uq8(r.u8());
    c.input.move_y = uq8(r.u8());
    c.input.buttons = r.u8();
    c.input.connected = true;
    out->cmds.push_back(c);
  }
  return r.ok() && r.at_end();
}

// --- Snapshot ---------------------------------------------------------------

std::vector<std::uint8_t> encode(const SnapshotMsg& m) {
  ByteWriter w = start(MsgType::Snapshot);
  w.varu(m.tick);
  w.varu(m.ack_seq);
  w.u32(m.echo_time_ms);
  w.varu(m.actors.size());
  for (const runtime::NetActor& a : m.actors) {
    std::uint8_t f = 0;
    if (a.hidden) f |= kRowHidden;
    if (a.collider_on) f |= kRowCollider;
    if (a.slot >= 0) f |= kRowRider;
    if (a.spawned) f |= kRowSpawned;
    if (a.facing < 0) f |= kRowFaceLeft;
    w.varu(a.id);
    w.u8(f);
    w.f32(a.x);
    w.f32(a.y);
    if (a.slot >= 0) {
      w.u8(static_cast<std::uint8_t>(a.slot));
      w.u8(static_cast<std::uint8_t>(q8(a.in_x)));
      w.u8(static_cast<std::uint8_t>(q8(a.in_y)));
      w.u8(static_cast<std::uint8_t>(a.dir));
    }
    if (a.spawned) w.str(a.name, 256);
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, SnapshotMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Snapshot, &r)) return false;
  out->tick = r.varu();
  out->ack_seq = static_cast<std::uint32_t>(r.varu());
  out->echo_time_ms = r.u32();
  const std::uint64_t n = r.varu();
  if (!r.ok() || n > kMaxSnapshotActors || n > r.remaining()) return false;
  out->actors.clear();
  out->actors.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    runtime::NetActor a;
    a.id = r.varu();
    const std::uint8_t f = r.u8();
    a.x = r.f32();
    a.y = r.f32();
    a.hidden = (f & kRowHidden) != 0;
    a.collider_on = (f & kRowCollider) != 0;
    a.spawned = (f & kRowSpawned) != 0;
    a.facing = (f & kRowFaceLeft) ? -1 : 1;
    if (f & kRowRider) {
      a.slot = r.u8();
      a.in_x = uq8(r.u8());
      a.in_y = uq8(r.u8());
      const std::uint8_t d = r.u8();
      if (a.slot >= kMaxPosse || d > 3) return false;
      a.dir = static_cast<runtime::Facing>(d);
    }
    if (a.spawned) a.name = r.str(256);
    if (!finite(a.x) || !finite(a.y) || a.id == 0) return false;
    out->actors.push_back(std::move(a));
  }
  return r.ok() && r.at_end();
}

// --- Event / Roster ---------------------------------------------------------

std::vector<std::uint8_t> encode(const EventMsg& m) {
  ByteWriter w = start(MsgType::Event);
  w.u8(static_cast<std::uint8_t>(m.kind));
  w.u8(static_cast<std::uint8_t>(m.slot + 1));
  w.u8(m.level);
  w.f32(m.seconds);
  w.str(m.channel, 64);
  w.str(m.text, 2048);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, EventMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Event, &r)) return false;
  const std::uint8_t k = r.u8();
  out->slot = static_cast<int>(r.u8()) - 1;
  out->level = r.u8();
  out->seconds = r.f32();
  out->channel = r.str(64);
  out->text = r.str(2048);
  if (k < 1 || k > 4 || out->slot >= kMaxPosse || !finite(out->seconds)) return false;
  out->kind = static_cast<EventMsg::Kind>(k);
  return r.ok();
}

std::vector<std::uint8_t> encode(const RosterMsg& m) {
  ByteWriter w = start(MsgType::Roster);
  w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(m.riders.size(), kMaxPosse)));
  for (std::size_t i = 0; i < m.riders.size() && i < kMaxPosse; ++i) {
    const RosterEntry& e = m.riders[i];
    w.u8(static_cast<std::uint8_t>(e.slot));
    w.str(e.name, kMaxNameLength * 4);
    w.u16(e.rtt_ms);
    w.varu(e.rider);
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& data, RosterMsg* out) {
  ByteReader r(data);
  if (!open(data, MsgType::Roster, &r)) return false;
  const std::size_t n = r.u8();
  if (n > kMaxPosse) return false;
  out->riders.clear();
  for (std::size_t i = 0; i < n; ++i) {
    RosterEntry e;
    e.slot = r.u8();
    e.name = r.str(kMaxNameLength * 4);
    e.rtt_ms = r.u16();
    e.rider = r.varu();
    if (e.slot >= kMaxPosse) return false;
    out->riders.push_back(std::move(e));
  }
  return r.ok();
}

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
