#include "team/TeamServer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace team {

namespace {

// Dusk: sunset orange, sage, dusty blue, brass, rose, sky, plum, rust.
const std::vector<std::string> kPalette = {"#e8833a", "#8fb573", "#6f93c4", "#d4b05a",
                                           "#d77a8a", "#62b8c9", "#a27ac2", "#c4553b"};

Value str(const std::string& s) { return Value::string(s); }
Value num(double d) { return Value::number(d); }

// A claim id as a file name (ids are GUID-ish; anything odd becomes _).
std::string file_of(const std::string& claim) {
  std::string out;
  for (char c : claim) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.';
    out.push_back(ok ? c : '_');
  }
  if (out.empty() || out[0] == '.') out = "_" + out;
  return out + ".team.json";
}

}  // namespace

std::vector<std::uint8_t> encode(const Value& msg) {
  const std::string s = write_compact(msg);
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

bool decode(const std::vector<std::uint8_t>& data, Value* out) {
  std::string err;
  return json::parse(std::string(data.begin(), data.end()), out, &err) && out->is_object();
}

const std::vector<std::string>& TeamServer::palette() { return kPalette; }

TeamServer::TeamServer() : t0_(std::chrono::steady_clock::now()) {}
TeamServer::~TeamServer() { stop(); }

double TeamServer::now() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
}

void TeamServer::log(const std::string& s) const {
  if (opt_.log) opt_.log("[team] " + s);
}

bool TeamServer::start(const ServerOptions& opt, std::string* error) {
  stop();
  opt_ = opt;
  std::unique_ptr<net::Transport> t = net::make_enet_transport();
  if (opt_.wrap_transport) t = opt_.wrap_transport(std::move(t));
  if (!t->listen(opt_.port, opt_.max_peers, error)) return false;
  transport_ = std::move(t);
  log("listening on UDP " + std::to_string(transport_->port()));
  load_sessions();
  return true;
}

// ---- Persistence ----

std::string TeamServer::new_token() {
  std::random_device rd;
  std::mt19937_64 rng((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ ++token_salt_ ^
                      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(rng()),
                static_cast<unsigned long long>(rng()));
  return buf;
}

void TeamServer::mark_dirty(Session& s) {
  if (opt_.state_dir.empty()) return;
  if (!s.dirty) s.dirty_at = now();
  s.dirty = true;
}

void TeamServer::save_session(Session& s) {
  s.dirty = false;
  if (opt_.state_dir.empty()) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::u8path(opt_.state_dir), ec);
  Value o = Value::object();
  o.set("format", num(1));
  o.set("proto", num(kProtocolVersion));
  o.set("claim", str(s.claim));
  o.set("claim_name", str(s.claim_name));
  o.set("engine", str(s.engine));
  o.set("dim", str(s.dim));
  Value formats = Value::object();
  formats.set("scene2d", num(s.scene2d));
  formats.set("scene3d", num(s.scene3d));
  o.set("formats", std::move(formats));
  o.set("seq", num(static_cast<double>(s.seq)));
  o.set("next_grant", id_value(s.next_grant));
  o.set("colour_next", num(s.colour_next));
  Value riders = Value::object();
  for (const auto& [tok, r] : s.riders) {
    Value rv = Value::object();
    rv.set("id", num(r.id));
    rv.set("name", str(r.name));
    rv.set("colour", str(r.colour));
    riders.set(tok, std::move(rv));
  }
  o.set("riders", std::move(riders));
  o.set("docs", docs_to_json(s.docs));
  const fs::path dest = fs::u8path(opt_.state_dir) / file_of(s.claim);
  const fs::path tmp = dest.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      log("could not save session '" + s.claim + "' to " + dest.string());
      return;
    }
    f << write_compact(o);
  }
  fs::rename(tmp, dest, ec);
  if (ec) {
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
  }
  ++total_saves_;
}

void TeamServer::save_all() {
  for (auto& [claim, s] : sessions_) {
    if (s.dirty) save_session(s);
  }
}

void TeamServer::load_sessions() {
  if (opt_.state_dir.empty()) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(fs::u8path(opt_.state_dir), ec)) return;
  for (const auto& entry : fs::directory_iterator(fs::u8path(opt_.state_dir), ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() < 10 || name.substr(name.size() - 10) != ".team.json") continue;
    std::ifstream f(entry.path(), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Value o;
    std::string err;
    if (!json::parse(ss.str(), &o, &err) || !o.is_object()) {
      log("skipping unreadable session file " + name + ": " + err);
      continue;
    }
    Session s;
    s.claim = o.str("claim");
    if (s.claim.empty()) continue;
    if (!opt_.only_claim.empty() && s.claim != opt_.only_claim) continue;
    s.claim_name = o.str("claim_name", s.claim);
    s.engine = o.str("engine");
    s.dim = o.str("dim");
    s.scene2d = static_cast<int>(o["formats"].num("scene2d", 0));
    s.scene3d = static_cast<int>(o["formats"].num("scene3d", 0));
    if (!docs_from_json(o["docs"], &s.docs)) {
      log("skipping session file " + name + ": bad docs");
      continue;
    }
    s.snap_docs = s.docs;
    s.seq = static_cast<std::uint64_t>(o.num("seq", 0));
    s.snap_seq = s.seq;
    s.next_grant = id_of(o["next_grant"]);
    s.colour_next = static_cast<int>(o.num("colour_next", 0));
    for (const auto& [tok, rv] : o["riders"].members()) {
      Session::Rider r;
      r.id = static_cast<std::uint32_t>(rv.num("id", 0));
      r.name = rv.str("name");
      r.colour = rv.str("colour");
      if (!r.id) continue;
      next_member_ = std::max(next_member_, r.id + 1);
      s.riders[tok] = std::move(r);
    }
    log("session for claim '" + s.claim + "' loaded (seq " + std::to_string(s.seq) + ")");
    sessions_[s.claim] = std::move(s);
  }
}

void TeamServer::stop() {
  if (!transport_) return;
  save_all();
  for (auto& [peer, m] : members_) transport_->disconnect(peer, 0);
  transport_->flush();
  transport_->close();
  transport_.reset();
  members_.clear();
  sessions_.clear();
}

std::uint16_t TeamServer::port() const { return transport_ ? transport_->port() : 0; }

void TeamServer::send(net::PeerId peer, const Value& msg, bool reliable) {
  if (!transport_) return;
  transport_->send(peer, reliable ? net::Delivery::Reliable : net::Delivery::Unreliable,
                   encode(msg));
}

void TeamServer::broadcast(const Session& s, const Value& msg, std::uint32_t except,
                           bool reliable) {
  const std::vector<std::uint8_t> bytes = encode(msg);
  for (auto& [peer, m] : members_) {
    if (!m.joined || m.claim != s.claim || m.id == except) continue;
    transport_->send(peer, reliable ? net::Delivery::Reliable : net::Delivery::Unreliable,
                     bytes);
  }
}

TeamServer::Member* TeamServer::member_by_peer(net::PeerId peer) {
  auto it = members_.find(peer);
  return it == members_.end() ? nullptr : &it->second;
}

Value TeamServer::member_json(const Member& m) const {
  Value o = Value::object();
  o.set("id", num(m.id));
  o.set("name", str(m.name));
  o.set("colour", str(m.colour));
  o.set("can_write", Value::boolean(m.can_write));
  o.set("scene", str(m.scene));
  return o;
}

void TeamServer::poll() {
  if (!transport_) return;
  net::TransportEvent ev;
  while (transport_->poll(&ev)) {
    switch (ev.type) {
      case net::TransportEvent::Type::Connected: {
        Member m;
        m.peer = ev.peer;
        m.last_seen = now();
        members_[ev.peer] = std::move(m);
        break;
      }
      case net::TransportEvent::Type::Disconnected: on_gone(ev.peer); break;
      case net::TransportEvent::Type::Received: {
        Member* m = member_by_peer(ev.peer);
        if (!m) break;
        Value msg;
        if (!decode(ev.data, &msg)) {
          log("bad message from peer " + std::to_string(ev.peer));
          break;
        }
        m->last_seen = now();
        on_message(*m, msg, ev.delivery == net::Delivery::Reliable);
        break;
      }
    }
  }
  // Silent members: locks go first, then the member.
  const double t = now();
  std::vector<net::PeerId> drop;
  for (auto& [peer, m] : members_) {
    if (!m.joined) {
      if (t - m.last_seen > opt_.member_timeout_s) drop.push_back(peer);
      continue;
    }
    auto it = sessions_.find(m.claim);
    if (it != sessions_.end() && t - m.last_seen > opt_.lock_timeout_s) {
      release_locks(it->second, m.id, true);
    }
    if (t - m.last_seen > opt_.member_timeout_s) drop.push_back(peer);
  }
  for (net::PeerId p : drop) {
    transport_->disconnect(p, 0);
    on_gone(p);
  }
  for (auto& [claim, s] : sessions_) {
    if (s.dirty && t - s.dirty_at >= opt_.save_delay_s) save_session(s);
  }
  transport_->flush();
}

void TeamServer::on_message(Member& m, const Value& msg, bool reliable) {
  const std::string t = msg.str("t");
  if (t == "hello") return on_hello(m, msg);
  if (!m.joined) return;
  if (t == "op") return on_op(m, msg);
  if (t == "lock") return on_lock(m, msg);
  if (t == "take") return on_take(m, msg);
  if (t == "pres") return on_presence(m, msg);
  if (t == "prev") return on_preview(m, msg, false);
  if (t == "prev_end") return on_preview(m, msg, true);
  if (t.rfind("blob_", 0) == 0) {
    Value r = Value::object();
    if (opt_.blobs) {
      r = opt_.blobs(m.claim, m.can_write, msg);
      if (!r.is_object()) r = Value::object();
    } else {
      r.set("ok", Value::boolean(false));
      r.set("error", str("This team host does not carry files over the session; use a Vault URL."));
    }
    r.set("t", str("blob_r"));
    r.set("rid", num(msg.num("rid", 0)));
    send(m.peer, r);
    return;
  }
  if (t == "bye") {
    transport_->disconnect(m.peer, 0);
    on_gone(m.peer);
    return;
  }
  (void)reliable;
}

void TeamServer::refuse(Member& m, const std::string& reason) {
  Value r = Value::object();
  r.set("t", str("refuse"));
  r.set("reason", str(reason));
  send(m.peer, r);
  transport_->disconnect(m.peer, 1);
  log("refused " + (m.name.empty() ? std::string("a rider") : m.name) + ": " + reason);
}

void TeamServer::on_hello(Member& m, const Value& msg) {
  if (m.joined) return;
  const int proto = static_cast<int>(msg.num("proto", 0));
  if (proto != kProtocolVersion) {
    return refuse(m, "Team protocol " + std::to_string(proto) + " is not spoken here (server speaks " +
                         std::to_string(kProtocolVersion) + "). Update TombStone to join.");
  }
  std::string name = msg.str("user");
  bool can_write = true;
  if (!opt_.passcode.empty()) {
    if (msg.str("pass") != opt_.passcode) return refuse(m, "Wrong passcode for this team claim.");
  } else if (opt_.check_key) {
    std::string key_name;
    if (!opt_.check_key(msg.str("key"), &key_name, &can_write)) {
      return refuse(m, "The Vault did not recognise that API key.");
    }
    if (name.empty()) name = key_name;
  }
  std::string claim = msg.str("claim");
  // "*" = whichever claim this host is running (Join from another claim).
  if (claim == "*") {
    if (!opt_.only_claim.empty()) claim = opt_.only_claim;
    else if (sessions_.size() == 1) claim = sessions_.begin()->first;
    else return refuse(m, "Name the claim to join: this server runs " +
                              std::to_string(sessions_.size()) + " team claims.");
    if (!sessions_.count(claim)) return refuse(m, "Nobody is riding that claim yet.");
  }
  if (claim.empty()) return refuse(m, "No claim named in the hello.");
  if (!opt_.only_claim.empty() && claim != opt_.only_claim) {
    return refuse(m, "This host is running a different claim.");
  }
  const int f2 = static_cast<int>(msg["formats"].num("scene2d", 0));
  const int f3 = static_cast<int>(msg["formats"].num("scene3d", 0));
  const std::string engine = msg.str("engine");
  std::string warn;

  auto it = sessions_.find(claim);
  if (it == sessions_.end()) {
    Session s;
    s.claim = claim;
    s.claim_name = msg.str("claim_name", claim);
    s.engine = engine;
    s.dim = msg.str("dim");
    s.scene2d = f2;
    s.scene3d = f3;
    Docs seed;
    if (msg.has("seed")) docs_from_json(msg["seed"], &seed);
    s.docs = seed;
    s.snap_docs = seed;
    it = sessions_.emplace(claim, std::move(s)).first;
    log("session opened for claim '" + claim + "' (" + it->second.dim + ", engine " + engine + ")");
  } else {
    Session& s = it->second;
    const std::string dim = msg.str("dim");
    const bool want3d = dim != "2d", have3d = s.dim != "2d";
    if (!dim.empty() && !s.dim.empty() && want3d != have3d) {
      return refuse(m, "That claim is " + s.dim + "; open a " + (have3d ? "3D" : "2D") +
                           " claim to join it.");
    }
    if (f2 != s.scene2d || f3 != s.scene3d) {
      return refuse(m, "This claim's posse rides scene formats 2D v" + std::to_string(s.scene2d) +
                           " / 3D v" + std::to_string(s.scene3d) + "; this editor writes 2D v" +
                           std::to_string(f2) + " / 3D v" + std::to_string(f3) +
                           ". Update TombStone to join.");
    }
    if (engine != s.engine) {
      warn = "Engine versions differ (session " + s.engine + ", yours " + engine +
             "). Scene formats match, so you can ride, but update when you can.";
    }
  }
  Session& s = it->second;
  // Riding back in (lost connection, editor or server restart): same
  // member id and colour when the token is known and nobody has the id.
  std::string token = msg.str("rejoin");
  const Session::Rider* back = nullptr;
  if (!token.empty()) {
    auto rit = s.riders.find(token);
    if (rit != s.riders.end()) {
      bool taken = false;
      for (const auto& [p, o] : members_) taken |= o.joined && o.id == rit->second.id;
      if (!taken) back = &rit->second;
    }
  }
  if (back) {
    m.id = back->id;
    m.colour = back->colour;
  } else {
    token = new_token();
    m.id = next_member_++;
    m.colour = kPalette[static_cast<std::size_t>(s.colour_next++) % kPalette.size()];
  }
  m.claim = claim;
  m.name = name.empty() ? (back && !back->name.empty() ? back->name : "Rider " + std::to_string(m.id)) : name;
  {
    Session::Rider& r = s.riders[token];
    r.id = m.id;
    r.name = m.name;
    r.colour = m.colour;
    r.last = now();
    // Keep the 64 most recent riders.
    while (s.riders.size() > 64) {
      auto oldest = s.riders.begin();
      for (auto i = s.riders.begin(); i != s.riders.end(); ++i) {
        if (i->second.last < oldest->second.last) oldest = i;
      }
      s.riders.erase(oldest);
    }
  }
  m.can_write = can_write;
  m.scene = msg.str("scene");
  m.joined = true;

  // Id range: above every id in the session and every earlier grant.
  std::uint64_t max_id = 0;
  for (const auto& [k, d] : s.docs) max_id = std::max(max_id, d.max_id());
  const std::uint64_t block = std::max<std::uint64_t>(opt_.id_block, 1024);
  std::uint64_t lo = (max_id / block + 1) * block;
  lo = std::max(lo, s.next_grant);
  s.next_grant = lo + block;
  mark_dirty(s);

  Value w = Value::object();
  w.set("t", str("welcome"));
  w.set("member", num(m.id));
  w.set("rejoin", str(token));
  if (back) w.set("back", Value::boolean(true));
  w.set("name", str(m.name));
  w.set("colour", str(m.colour));
  w.set("can_write", Value::boolean(m.can_write));
  Value sess = Value::object();
  sess.set("claim", str(s.claim));
  sess.set("claim_name", str(s.claim_name));
  sess.set("engine", str(s.engine));
  sess.set("dim", str(s.dim));
  Value formats = Value::object();
  formats.set("scene2d", num(s.scene2d));
  formats.set("scene3d", num(s.scene3d));
  sess.set("formats", std::move(formats));
  if (!opt_.files.is_null()) sess.set("files", opt_.files);
  w.set("session", std::move(sess));
  Value ids = Value::object();
  ids.set("lo", id_value(lo));
  ids.set("hi", id_value(lo + block));
  w.set("ids", std::move(ids));
  w.set("snap_seq", num(static_cast<double>(s.snap_seq)));
  w.set("seq", num(static_cast<double>(s.seq)));
  w.set("docs", docs_to_json(s.snap_docs));
  Value tail = Value::array();
  for (const Batch& b : s.tail) tail.push(batch_to_json(b));
  w.set("tail", std::move(tail));
  Value members = Value::array();
  for (const auto& [peer, o] : members_) {
    if (o.joined && o.claim == claim) members.push(member_json(o));
  }
  w.set("members", std::move(members));
  Value locks = Value::object();
  for (const auto& [k, id] : s.locks) locks.set(k, num(id));
  w.set("locks", std::move(locks));
  if (!warn.empty()) w.set("warn", str(warn));
  send(m.peer, w);

  Value j = Value::object();
  j.set("t", str("member"));
  j.set("m", member_json(m));
  broadcast(s, j, m.id);
  log(m.name + (back ? " rode back in to '" : " rode in to '") + claim + "' (member " + std::to_string(m.id) + ")");
}

void TeamServer::on_op(Member& m, const Value& msg) {
  Session& s = sessions_[m.claim];
  Batch b;
  auto reject = [&](const std::string& reason, std::uint32_t by) {
    Value r = Value::object();
    r.set("t", str("reject"));
    r.set("cid", num(static_cast<double>(msg.num("cid", 0))));
    r.set("reason", str(reason));
    if (by) r.set("by", num(by));
    send(m.peer, r);
    ++total_rejects_;
  };
  if (!batch_from_json(msg, &b)) return reject("Malformed edit.", 0);
  if (!m.can_write) return reject("Your key is read-only: you can watch, not edit.", 0);
  for (const Op& op : b.ops) {
    std::string key;
    if (op.kind == Op::Kind::Put || op.kind == Op::Kind::Del) key = entity_lock(b.scene, op.id);
    else if (op.kind == Op::Kind::Terrain && op.id) key = entity_lock(b.scene, op.id);
    else if (op.kind == Op::Kind::Asset) key = asset_lock(op.key);
    if (key.empty()) continue;
    auto it = s.locks.find(key);
    if (it != s.locks.end() && it->second != m.id) {
      std::string who = "another rider";
      for (const auto& [p, o] : members_) {
        if (o.id == it->second) who = o.name;
      }
      return reject("Locked by " + who, it->second);
    }
  }
  for (const Op& op : b.ops) {
    if (op.kind != Op::Kind::Put && op.kind != Op::Kind::Del && op.kind != Op::Kind::Terrain) continue;
    const std::string key = entity_lock(b.scene, op.id);
    auto it = s.locks.find(key);
    if (it != s.locks.end() && it->second == m.id) touch_lock(s, key);
  }
  b.seq = ++s.seq;
  b.from = m.id;
  s.docs[b.scene].apply(b);
  s.tail.push_back(b);
  mark_dirty(s);
  ++total_commits_;
  Value c = batch_to_json(b);
  c.set("t", str("commit"));
  broadcast(s, c);
  // A deleted entity (or file) holds no lock any more.
  bool unlocked = false;
  for (const Op& op : b.ops) {
    std::string key;
    if (op.kind == Op::Kind::Del) key = entity_lock(b.scene, op.id);
    else if (op.kind == Op::Kind::Asset && op.sha256.empty()) key = asset_lock(op.key);
    if (!key.empty()) unlocked |= s.locks.erase(key) > 0;
  }
  if (unlocked) send_locks(s);
  if (s.tail.size() >= std::max<std::size_t>(opt_.compact_every, 1)) {
    s.snap_docs = s.docs;
    s.snap_seq = s.seq;
    s.tail.clear();
  }
}

void TeamServer::send_locks(const Session& s) {
  Value l = Value::object();
  l.set("t", str("locks"));
  Value locks = Value::object();
  for (const auto& [k, id] : s.locks) locks.set(k, num(id));
  l.set("locks", std::move(locks));
  broadcast(s, l);
}

void TeamServer::on_lock(Member& m, const Value& msg) {
  Session& s = sessions_[m.claim];
  std::set<std::string> want;
  for (const Value& k : msg["keys"].items()) {
    if (k.is_string() && !k.as_string().empty()) want.insert(k.as_string());
  }
  bool changed = false;
  for (auto it = s.locks.begin(); it != s.locks.end();) {
    if (it->second == m.id && !want.count(it->first)) {
      it = s.locks.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  Value denied = Value::array();
  std::uint32_t by = 0;
  for (const std::string& k : want) {
    auto it = s.locks.find(k);
    if (it == s.locks.end()) {
      if (!m.can_write) continue;  // viewers never hold locks
      s.locks[k] = m.id;
      touch_lock(s, k);
      changed = true;
    } else if (it->second != m.id) {
      denied.push(str(k));
      by = it->second;
    }
  }
  if (changed) send_locks(s);
  if (denied.size()) {
    Value d = Value::object();
    d.set("t", str("lock_denied"));
    d.set("keys", std::move(denied));
    d.set("by", num(by));
    send(m.peer, d);
  }
}

void TeamServer::on_take(Member& m, const Value& msg) {
  Session& s = sessions_[m.claim];
  auto name_of = [&](std::uint32_t id) {
    for (const auto& [p, o] : members_) {
      if (o.id == id) return o.name;
    }
    return std::string("another rider");
  };
  bool changed = false;
  Value denied = Value::array();
  std::string reason;
  std::uint32_t by = 0;
  const double t = now();
  for (const Value& kv : msg["keys"].items()) {
    if (!kv.is_string() || kv.as_string().empty()) continue;
    const std::string k = kv.as_string();
    if (!m.can_write) {
      reason = "Viewers can't take locks.";
      denied.push(str(k));
      continue;
    }
    auto it = s.locks.find(k);
    if (it == s.locks.end()) {
      s.locks[k] = m.id;
      touch_lock(s, k);
      changed = true;
      continue;
    }
    if (it->second == m.id) continue;
    const double used = s.lock_used.count(k) ? s.lock_used[k] : 0.0;
    const double idle = t - used;
    if (opt_.lock_idle_s <= 0.0 || idle < opt_.lock_idle_s) {
      by = it->second;
      reason = name_of(by) + " is still working on it (busy " + std::to_string(static_cast<int>(idle)) +
               " s ago; take-over after " + std::to_string(static_cast<int>(opt_.lock_idle_s)) + " s idle).";
      denied.push(str(k));
      continue;
    }
    const std::uint32_t old = it->second;
    it->second = m.id;
    touch_lock(s, k);
    changed = true;
    Value n = Value::object();
    n.set("t", str("taken"));
    n.set("key", str(k));
    n.set("by", num(m.id));
    n.set("from", num(old));
    broadcast(s, n);
    log(m.name + " took over " + k + " from " + name_of(old));
  }
  if (changed) send_locks(s);
  if (denied.size()) {
    Value d = Value::object();
    d.set("t", str("lock_denied"));
    d.set("keys", std::move(denied));
    d.set("by", num(by));
    d.set("reason", str(reason));
    send(m.peer, d);
  }
}

void TeamServer::on_presence(Member& m, const Value& msg) {
  Session& s = sessions_[m.claim];
  const std::string scene = msg.str("scene", m.scene);
  const bool scene_changed = scene != m.scene;
  m.scene = scene;
  m.presence = msg;
  Value p = msg;
  p.set("member", num(m.id));
  broadcast(s, p, m.id, false);
  if (scene_changed) {
    Value j = Value::object();
    j.set("t", str("member"));
    j.set("m", member_json(m));
    broadcast(s, j);
  }
}

void TeamServer::on_preview(Member& m, const Value& msg, bool end) {
  Session& s = sessions_[m.claim];
  if (!m.can_write) return;  // viewers watch, they don't drag
  const std::string scene = msg.str("scene", m.scene);
  Value out = Value::object();
  out.set("t", str(end ? "prev_end" : "prev"));
  out.set("member", num(m.id));
  out.set("scene", str(scene));
  out.set("n", num(msg.num("n", 0)));
  if (end) {
    out.set("ids", msg["ids"]);
    broadcast(s, out, m.id);
    return;
  }
  // Only entities this member holds: others see the drag of a locked
  // entity, never a preview of something they could be editing.
  Value ops = Value::array();
  for (const Value& ov : msg["ops"].items()) {
    Op op;
    if (!op_from_json(ov, &op) || op.kind != Op::Kind::Put) continue;
    const std::string key = entity_lock(scene, op.id);
    auto it = s.locks.find(key);
    if (it == s.locks.end() || it->second != m.id) continue;
    touch_lock(s, key);
    ops.push(ov);
  }
  if (!ops.size()) return;
  out.set("ops", std::move(ops));
  broadcast(s, out, m.id, false);
}

void TeamServer::release_locks(Session& s, std::uint32_t member, bool notify) {
  bool changed = false;
  for (auto it = s.locks.begin(); it != s.locks.end();) {
    if (it->second == member) {
      it = s.locks.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed && notify) send_locks(s);
}

void TeamServer::on_gone(net::PeerId peer) {
  auto it = members_.find(peer);
  if (it == members_.end()) return;
  Member m = std::move(it->second);
  members_.erase(it);
  if (!m.joined) return;
  auto sit = sessions_.find(m.claim);
  if (sit == sessions_.end()) return;
  Session& s = sit->second;
  release_locks(s, m.id, true);
  Value l = Value::object();
  l.set("t", str("left"));
  l.set("member", num(m.id));
  broadcast(s, l);
  log(m.name + " rode off from '" + m.claim + "'");
  // The session stays while the server runs (stage 3 persists it), so a
  // posse that all drops out can ride back in to the same state.
}

std::vector<SessionInfo> TeamServer::sessions() const {
  std::vector<SessionInfo> out;
  for (const auto& [claim, s] : sessions_) {
    SessionInfo i;
    i.claim = s.claim;
    i.claim_name = s.claim_name;
    i.engine = s.engine;
    i.scene2d = s.scene2d;
    i.scene3d = s.scene3d;
    i.dim = s.dim;
    i.seq = s.seq;
    i.snap_seq = s.snap_seq;
    i.locks = s.locks;
    for (const auto& [peer, m] : members_) {
      if (m.joined && m.claim == claim) {
        i.members.push_back({m.id, m.name, m.colour, m.can_write, m.scene, m.presence});
      }
    }
    out.push_back(std::move(i));
  }
  return out;
}

const Docs* TeamServer::docs(const std::string& claim) const {
  auto it = sessions_.find(claim);
  return it == sessions_.end() ? nullptr : &it->second.docs;
}

Value TeamServer::info_json() const {
  Value o = Value::object();
  o.set("enabled", Value::boolean(running()));
  o.set("port", num(port()));
  o.set("proto", num(kProtocolVersion));
  Value arr = Value::array();
  for (const SessionInfo& i : sessions()) {
    Value s = Value::object();
    s.set("claim", str(i.claim));
    s.set("claim_name", str(i.claim_name));
    s.set("engine", str(i.engine));
    s.set("dim", str(i.dim));
    Value formats = Value::object();
    formats.set("scene2d", num(i.scene2d));
    formats.set("scene3d", num(i.scene3d));
    s.set("formats", std::move(formats));
    s.set("seq", num(static_cast<double>(i.seq)));
    Value ms = Value::array();
    for (const MemberInfo& m : i.members) {
      Value mj = Value::object();
      mj.set("name", str(m.name));
      mj.set("colour", str(m.colour));
      mj.set("scene", str(m.scene));
      ms.push(std::move(mj));
    }
    s.set("members", std::move(ms));
    arr.push(std::move(s));
  }
  o.set("sessions", std::move(arr));
  return o;
}

}  // namespace team
}  // namespace tombstone
}  // namespace ts
