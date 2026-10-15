#include "mmo/MmoProtocol.h"

#include "net/ByteStream.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace mmo {

using net::ByteReader;
using net::ByteWriter;

namespace {

constexpr std::size_t kMaxRows = 8192;
constexpr std::size_t kMaxGeo = 4096;
constexpr std::size_t kMaxItems = 1024;
constexpr std::size_t kMaxName = 64;

ByteWriter start(MmoMsg t) {
  ByteWriter w;
  w.u8(static_cast<std::uint8_t>(t));
  return w;
}

bool open(const std::vector<std::uint8_t>& d, MmoMsg t, ByteReader* r) {
  if (d.empty() || d[0] != static_cast<std::uint8_t>(t)) return false;
  r->u8();
  return true;
}

bool finite(float v) { return std::isfinite(v); }

void put_rect(ByteWriter& w, const runtime::Aabb& a) {
  w.f32(a.x0);
  w.f32(a.y0);
  w.f32(a.x1);
  w.f32(a.y1);
}

bool get_rect(ByteReader& r, runtime::Aabb* a) {
  a->x0 = r.f32();
  a->y0 = r.f32();
  a->x1 = r.f32();
  a->y1 = r.f32();
  return finite(a->x0) && finite(a->y0) && finite(a->x1) && finite(a->y1);
}

}  // namespace

bool peek(const std::vector<std::uint8_t>& data, MmoMsg* out) {
  if (data.empty() || data[0] < 64 || data[0] > 75) return false;
  *out = static_cast<MmoMsg>(data[0]);
  return true;
}

std::vector<std::uint8_t> encode(const LoginMsg& m) {
  ByteWriter w = start(MmoMsg::Login);
  w.u16(m.version);
  w.u8(static_cast<std::uint8_t>(m.mode));
  w.str(m.user, kMaxUser * 4);
  w.str(m.password, kMaxPassword * 4);
  w.str(m.character, kMaxUser * 4);
  w.str(m.token, 128);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, LoginMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Login, &r)) return false;
  m->version = r.u16();
  const std::uint8_t mode = r.u8();
  if (mode > 2) return false;
  m->mode = static_cast<LoginMode>(mode);
  m->user = r.str(kMaxUser * 4);
  m->password = r.str(kMaxPassword * 4);
  m->character = r.str(kMaxUser * 4);
  m->token = r.str(128);
  return r.at_end();
}

std::vector<std::uint8_t> encode(const LoginOkMsg& m) {
  ByteWriter w = start(MmoMsg::LoginOk);
  w.u64(m.entity);
  w.str(m.name, kMaxName);
  w.str(m.token, 128);
  w.u64(m.tick);
  w.u8(static_cast<std::uint8_t>(m.tick_rate));
  w.f32(m.speed);
  w.f32(m.interest_radius);
  w.u8((m.admin ? 1 : 0) | (m.resumed ? 2 : 0));
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, LoginOkMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::LoginOk, &r)) return false;
  m->entity = r.u64();
  m->name = r.str(kMaxName);
  m->token = r.str(128);
  m->tick = r.u64();
  m->tick_rate = r.u8();
  m->speed = r.f32();
  m->interest_radius = r.f32();
  const std::uint8_t f = r.u8();
  m->admin = f & 1;
  m->resumed = f & 2;
  return r.at_end() && m->tick_rate > 0 && finite(m->speed) && finite(m->interest_radius);
}

std::vector<std::uint8_t> encode(const LoginFailMsg& m) {
  ByteWriter w = start(MmoMsg::LoginFail);
  w.u8(m.code);
  w.str(m.text, 512);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, LoginFailMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::LoginFail, &r)) return false;
  m->code = r.u8();
  m->text = r.str(512);
  return r.at_end();
}

std::vector<std::uint8_t> encode(const ZoneMsg& m) {
  ByteWriter w = start(MmoMsg::Zone);
  w.str(m.geo.id, kMaxName);
  w.str(m.geo.name, kMaxName);
  put_rect(w, m.geo.bounds);
  w.varu(m.geo.blockers.size());
  for (const auto& b : m.geo.blockers) put_rect(w, b);
  w.varu(m.geo.portals.size());
  for (const auto& p : m.geo.portals) {
    put_rect(w, p.rect);
    w.str(p.to, kMaxName);
    w.f32(p.at_x);
    w.f32(p.at_y);
  }
  w.f32(m.x);
  w.f32(m.y);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, ZoneMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Zone, &r)) return false;
  m->geo = ZoneGeometry{};
  m->geo.id = r.str(kMaxName);
  m->geo.name = r.str(kMaxName);
  if (!get_rect(r, &m->geo.bounds)) return false;
  const std::uint64_t nb = r.varu();
  if (nb > kMaxGeo) return false;
  for (std::uint64_t i = 0; i < nb && r.ok(); ++i) {
    runtime::Aabb a;
    if (!get_rect(r, &a)) return false;
    m->geo.blockers.push_back(a);
  }
  const std::uint64_t np = r.varu();
  if (np > kMaxGeo) return false;
  for (std::uint64_t i = 0; i < np && r.ok(); ++i) {
    Portal p;
    if (!get_rect(r, &p.rect)) return false;
    p.to = r.str(kMaxName);
    p.at_x = r.f32();
    p.at_y = r.f32();
    m->geo.portals.push_back(p);
  }
  m->x = r.f32();
  m->y = r.f32();
  return r.at_end() && finite(m->x) && finite(m->y);
}

std::vector<std::uint8_t> encode(const MoveMsg& m) {
  ByteWriter w = start(MmoMsg::Move);
  w.u32(m.client_ms);
  w.u16(m.rtt_ms);
  w.f32(m.claim_x);
  w.f32(m.claim_y);
  w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(m.cmds.size(), 16)));
  if (!m.cmds.empty()) w.u32(m.cmds[0].seq);
  for (std::size_t i = 0; i < m.cmds.size() && i < 16; ++i) {
    // Intent quantised to 1/127 per axis.
    w.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(std::clamp(m.cmds[i].mx, -1.0f, 1.0f) * 127.0f))));
    w.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(std::clamp(m.cmds[i].my, -1.0f, 1.0f) * 127.0f))));
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, MoveMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Move, &r)) return false;
  m->client_ms = r.u32();
  m->rtt_ms = r.u16();
  m->claim_x = r.f32();
  m->claim_y = r.f32();
  const std::uint8_t n = r.u8();
  if (n > 16) return false;
  m->cmds.clear();
  const std::uint32_t seq = n ? r.u32() : 0;
  for (std::uint8_t i = 0; i < n && r.ok(); ++i) {
    MoveCmd c;
    c.seq = seq - i;
    c.mx = static_cast<std::int8_t>(r.u8()) / 127.0f;
    c.my = static_cast<std::int8_t>(r.u8()) / 127.0f;
    m->cmds.push_back(c);
  }
  return r.at_end() && finite(m->claim_x) && finite(m->claim_y);
}

// Quantise intent the same way on the sending side (prediction must match).
std::vector<std::uint8_t> encode(const SnapshotMsg& m) {
  ByteWriter w = start(MmoMsg::Snapshot);
  w.u64(m.tick);
  w.u32(m.ack);
  w.f32(m.x);
  w.f32(m.y);
  w.u8(m.corrected ? 1 : 0);
  w.u32(m.echo_ms);
  w.varu(m.rows.size());
  for (const SnapRow& row : m.rows) {
    w.varu(row.id);
    w.f32(row.x);
    w.f32(row.y);
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, SnapshotMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Snapshot, &r)) return false;
  m->tick = r.u64();
  m->ack = r.u32();
  m->x = r.f32();
  m->y = r.f32();
  m->corrected = r.u8() != 0;
  m->echo_ms = r.u32();
  const std::uint64_t n = r.varu();
  if (n > kMaxRows) return false;
  m->rows.clear();
  m->rows.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    SnapRow row;
    row.id = r.varu();
    row.x = r.f32();
    row.y = r.f32();
    if (!finite(row.x) || !finite(row.y)) return false;
    m->rows.push_back(row);
  }
  return r.at_end() && finite(m->x) && finite(m->y);
}

std::vector<std::uint8_t> encode(const AppearMsg& m) {
  ByteWriter w = start(MmoMsg::Appear);
  w.varu(m.rows.size());
  for (const AppearRow& a : m.rows) {
    w.varu(a.id);
    w.u8(static_cast<std::uint8_t>(a.kind));
    w.str(a.name, kMaxName);
    w.f32(a.x);
    w.f32(a.y);
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, AppearMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Appear, &r)) return false;
  const std::uint64_t n = r.varu();
  if (n > kMaxRows) return false;
  m->rows.clear();
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    AppearRow a;
    a.id = r.varu();
    const std::uint8_t k = r.u8();
    if (k > 2) return false;
    a.kind = static_cast<EntityKind>(k);
    a.name = r.str(kMaxName);
    a.x = r.f32();
    a.y = r.f32();
    m->rows.push_back(std::move(a));
  }
  return r.at_end();
}

std::vector<std::uint8_t> encode(const VanishMsg& m) {
  ByteWriter w = start(MmoMsg::Vanish);
  w.varu(m.ids.size());
  for (std::uint64_t id : m.ids) w.varu(id);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, VanishMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Vanish, &r)) return false;
  const std::uint64_t n = r.varu();
  if (n > kMaxRows) return false;
  m->ids.clear();
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) m->ids.push_back(r.varu());
  return r.at_end();
}

std::vector<std::uint8_t> encode(const ChatSendMsg& m) {
  ByteWriter w = start(MmoMsg::ChatSend);
  w.str(m.channel, kMaxName);
  w.str(m.text, kMaxChat);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, ChatSendMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::ChatSend, &r)) return false;
  m->channel = r.str(kMaxName);
  m->text = r.str(kMaxChat);
  return r.at_end();
}

std::vector<std::uint8_t> encode(const ChatMsg& m) {
  ByteWriter w = start(MmoMsg::Chat);
  w.str(m.channel, kMaxName);
  w.str(m.from, kMaxName);
  w.str(m.text, kMaxChat * 8);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, ChatMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Chat, &r)) return false;
  m->channel = r.str(kMaxName);
  m->from = r.str(kMaxName);
  m->text = r.str(kMaxChat * 8);
  return r.at_end();
}

std::vector<std::uint8_t> encode(const InteractMsg& m) {
  ByteWriter w = start(MmoMsg::Interact);
  w.u64(m.entity);
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, InteractMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Interact, &r)) return false;
  m->entity = r.u64();
  return r.at_end();
}

std::vector<std::uint8_t> encode(const InventoryMsg& m) {
  ByteWriter w = start(MmoMsg::Inventory);
  w.varu(m.items.size());
  for (const auto& [k, v] : m.items) {
    w.str(k, kMaxName);
    w.vari(v);
  }
  return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& d, InventoryMsg* m) {
  ByteReader r(d);
  if (!open(d, MmoMsg::Inventory, &r)) return false;
  const std::uint64_t n = r.varu();
  if (n > kMaxItems) return false;
  m->items.clear();
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    std::string k = r.str(kMaxName);
    m->items[k] = static_cast<int>(r.vari());
  }
  return r.at_end();
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
