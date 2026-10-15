#include "mmo/MmoServer.h"
#include "mmo/MmoServerState.h"

#include "mmo/MmoCrypto.h"
#include "profiler/Profiler.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "scene/JsonTree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace ts {
namespace tombstone {
namespace mmo {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// State

namespace {

double wall_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> split_words(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

std::string rest_after(const std::string& s, int words) {
  std::size_t i = 0;
  for (int w = 0; w < words; ++w) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}

std::string fmt1(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

}  // namespace

bool valid_username(const std::string& s) {
  if (s.size() < 3 || s.size() > kMaxUser) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
  return true;
}

std::string clean_chat(const std::string& s, std::size_t max_len) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 32 || c == 127) continue;
    out.push_back(static_cast<char>(c));
  }
  const auto a = out.find_first_not_of(' ');
  if (a == std::string::npos) return {};
  out = out.substr(a, out.find_last_not_of(' ') - a + 1);
  if (out.size() > max_len) out.resize(max_len);
  return out;
}

float quantize_intent(float v) {
  if (!std::isfinite(v)) return 0.0f;
  return static_cast<float>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)) / 127.0f;
}

MmoServer::MmoServer() { register_admin(); }
MmoServer::~MmoServer() { stop(); }

void MmoServer::log(std::string line) {
  log_.push_back(std::move(line));
  if (log_.size() > 2000) log_.erase(log_.begin(), log_.begin() + 1000);
}

std::vector<std::string> MmoServer::take_log() {
  std::vector<std::string> out;
  out.swap(log_);
  return out;
}

void MmoServer::send(net::PeerId peer, net::Delivery d, const std::vector<std::uint8_t>& msg) {
  if (!transport_ || peer == net::kNoPeer) return;
  if (transport_->send(peer, d, msg)) bytes_out_ += msg.size();
}

std::uint16_t MmoServer::port() const { return transport_ ? transport_->port() : 0; }

// ---------------------------------------------------------------------------
// Start / stop

bool MmoServer::start(MmoServerConfig config, std::string* error) {
  stop();
  config_ = std::move(config);
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    zones_.clear();
    store_.close();
    transport_.reset();
    return false;
  };
  std::string err;
  if (!load_world_config(config_.project_dir, &cfg_, &err)) return fail(err);
  if (config_.argon2_kib > 0) cfg_.argon2_kib = config_.argon2_kib;
  if (config_.linkdead_seconds >= 0.0) cfg_.linkdead_seconds = config_.linkdead_seconds;
  if (config_.save_every > 0.0) cfg_.save_every = config_.save_every;
  const std::string db = !config_.db_path.empty()
                             ? config_.db_path
                             : (fs::path(config_.project_dir) / "mmo" / "world.db").string();
  if (!store_.open(db, &err)) return fail(err);
  store_.prune_sessions();
  quest_template_ = story::QuestLog{};
  quest_template_.load_dir(config_.project_dir);
  zones_.clear();
  for (const ZoneDef& def : cfg_.zones) {
    auto z = std::make_unique<ZoneRt>();
    z->def = &def;
    if (!def.entities.empty()) {
      z->world = std::make_unique<runtime::World>();
      z->world->set_net_role(runtime::NetRole::Server);
      std::string werr;
      if (!z->world->build(def.entities, def.solidity, config_.project_dir, &werr)) {
        log("zone " + def.geo.id + ": its World would not build (" + werr + "); riders only");
        z->world.reset();
      } else {
        for (const auto& a : z->world->actors()) z->actor_names[a.data.id] = a.data.name;
      }
    }
    for (const ItemSpawn& it : def.items) z->items.push_back(ZoneRt::Item{it, 0.0});
    zones_.push_back(std::move(z));
  }
  transport_ = config_.transport ? std::move(config_.transport) : net::make_enet_transport();
  if (!transport_->listen(config_.port, static_cast<std::size_t>(cfg_.max_players) + 32, &err))
    return fail(err.empty() ? "could not listen" : err);
  running_ = true;
  now_ = 0.0;
  accumulator_ = 0.0;
  save_timer_ = 0.0;
  shutdown_at_ = -1.0;
  stats_ = MmoStats{};
  stats_.accounts = store_.account_count();
  stats_.characters = store_.character_count();
  log("Town Hall open on UDP " + std::to_string(port()) + ": " + std::to_string(zones_.size()) +
      " zone(s), " + std::to_string(stats_.accounts) + " account(s), database " + db);
  return true;
}

void MmoServer::stop() {
  if (!running_) {
    transport_.reset();
    store_.close();
    return;
  }
  const int saved = save_all();
  for (auto& [id, c] : chars_) {
    if (c->peer != net::kNoPeer) transport_->disconnect(c->peer, kMmoShutdown);
    if (store_.is_open() && !c->token_digest.empty())
      store_.put_session(c->token_digest, c->id, unix_now() + static_cast<std::int64_t>(cfg_.linkdead_seconds) + 60);
  }
  for (int i = 0; i < 20; ++i) {
    transport_->flush();
    net::TransportEvent ev;
    while (transport_->poll(&ev)) {
    }
  }
  transport_->close();
  transport_.reset();
  chars_.clear();
  peers_.clear();
  zones_.clear();
  store_.close();
  running_ = false;
  log("Town Hall closed (" + std::to_string(saved) + " character(s) saved)");
}

// ---------------------------------------------------------------------------
// Loop

int MmoServer::update(double real_seconds) {
  if (!running_) return 0;
  const double dt = 1.0 / cfg_.tick_rate;
  accumulator_ += std::clamp(real_seconds, 0.0, 0.5);
  int n = 0;
  while (accumulator_ >= dt && n < 4) {
    accumulator_ -= dt;
    tick_once();
    ++n;
  }
  if (n == 0) {
    // Keep the socket serviced between ticks.
    net::TransportEvent ev;
    while (transport_ && transport_->poll(&ev)) handle(ev);
    if (transport_) transport_->flush();
  }
  return n;
}

void MmoServer::tick_once() {
  if (!running_) return;
  TS_PROFILE_SCOPE("mmo.tick");
  const double t0 = wall_ms();
  const double dt = 1.0 / cfg_.tick_rate;
  now_ += dt;
  ++stats_.ticks;
  {
    TS_PROFILE_SCOPE("mmo.net_in");
    net::TransportEvent ev;
    while (transport_->poll(&ev)) handle(ev);
  }
  {
    TS_PROFILE_SCOPE("mmo.move");
    move_riders(dt);
  }
  {
    std::vector<std::int64_t> kicked;
    for (auto& [id, c] : chars_)
      if (c->kick) kicked.push_back(id);
    for (std::int64_t id : kicked) disconnect_character(*chars_[id], chars_[id]->kick);
  }
  {
    TS_PROFILE_SCOPE("mmo.zones");
    step_zones(dt);
  }
  // Link-dead riders whose time is up.
  std::vector<std::int64_t> gone;
  for (auto& [id, c] : chars_)
    if (c->peer == net::kNoPeer && c->linkdead_since >= 0.0 && now_ - c->linkdead_since >= cfg_.linkdead_seconds)
      gone.push_back(id);
  for (std::int64_t id : gone) {
    log(chars_[id]->name + " was link-dead too long; saved and gone");
    logout(id);
  }
  {
    TS_PROFILE_SCOPE("mmo.replicate");
    replicate();
  }
  save_timer_ += dt;
  if (save_timer_ >= cfg_.save_every) {
    save_timer_ = 0.0;
    TS_PROFILE_SCOPE("mmo.save");
    save_all();
    store_.prune_sessions();
  }
  transport_->flush();
  measure(dt);
  const double ms = wall_ms() - t0;
  tick_ms_sum_ += ms;
  ++tick_ms_n_;
  stats_.tick_ms_max = std::max(stats_.tick_ms_max * 0.995, ms);
}

void MmoServer::measure(double dt) {
  measure_window_ += dt;
  if (measure_window_ < 1.0) return;
  stats_.kbps_out = static_cast<double>(bytes_out_) / 1024.0 / measure_window_;
  stats_.kbps_in = static_cast<double>(bytes_in_) / 1024.0 / measure_window_;
  bytes_out_ = bytes_in_ = 0;
  stats_.tick_ms_avg = tick_ms_n_ ? tick_ms_sum_ / tick_ms_n_ : 0.0;
  tick_ms_sum_ = 0.0;
  tick_ms_n_ = 0;
  measure_window_ = 0.0;
  profiler::Profiler& p = profiler::Profiler::get();
  if (p.enabled()) {
    p.counter("mmo.online", stats_.online);
    p.counter("mmo.linkdead", stats_.linkdead);
    p.counter("mmo.kbps_out", stats_.kbps_out);
    p.counter("mmo.kbps_in", stats_.kbps_in);
    p.counter("mmo.tick_ms", stats_.tick_ms_avg);
    p.counter("mmo.avg_visible", stats_.avg_visible);
  }
}

// ---------------------------------------------------------------------------
// Network

void MmoServer::handle(const net::TransportEvent& ev) {
  using T = net::TransportEvent::Type;
  if (ev.type == T::Connected) {
    PeerRt p;
    p.id = ev.peer;
    p.connected_at = now_;
    peers_[ev.peer] = p;
    return;
  }
  auto it = peers_.find(ev.peer);
  if (ev.type == T::Disconnected) {
    if (it != peers_.end()) {
      const std::int64_t cid = it->second.character;
      peers_.erase(it);
      auto ci = chars_.find(cid);
      if (ci != chars_.end() && ci->second->peer == ev.peer) {
        if (ev.reason == kMmoQuit) {
          log(ci->second->name + " logged out");
          logout(cid);
        } else {
          go_linkdead(*ci->second);
        }
      }
    }
    return;
  }
  if (it == peers_.end()) return;
  bytes_in_ += ev.data.size();
  PeerRt& p = it->second;
  MmoMsg type;
  if (!peek(ev.data, &type)) {
    transport_->disconnect(p.id, kMmoBadMessage);
    return;
  }
  if (p.character == 0) {
    LoginMsg m;
    if (type != MmoMsg::Login || !decode(ev.data, &m)) {
      transport_->disconnect(p.id, kMmoBadMessage);
      return;
    }
    on_login(p, m);
    return;
  }
  auto ci = chars_.find(p.character);
  if (ci == chars_.end()) return;
  Character& c = *ci->second;
  switch (type) {
    case MmoMsg::Move: {
      MoveMsg m;
      if (decode(ev.data, &m)) on_move(c, m);
      break;
    }
    case MmoMsg::ChatSend: {
      ChatSendMsg m;
      if (decode(ev.data, &m)) on_chat(c, m);
      break;
    }
    case MmoMsg::Interact: {
      InteractMsg m;
      if (decode(ev.data, &m)) on_interact(c, m);
      break;
    }
    default:
      transport_->disconnect(p.id, kMmoBadMessage);
      break;
  }
}

void MmoServer::login_fail(PeerRt& p, std::uint8_t code, const std::string& text, bool count) {
  LoginFailMsg f;
  f.code = code;
  f.text = text;
  send(p.id, net::Delivery::Reliable, encode(f));
  if (count) {
    ++stats_.failed_logins;
    if (++p.failed >= 5) transport_->disconnect(p.id, kMmoLoginFailed);
  }
}

bool MmoServer::create_account(const std::string& user, const std::string& password, bool admin,
                               const std::string& character, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (!valid_username(user)) return fail("usernames are 3-24 letters, digits, '_' or '-'");
  if (password.size() < 6 || password.size() > kMaxPassword) return fail("passwords are 6-128 characters");
  const std::string name = character.empty() ? user : character;
  if (!valid_username(name)) return fail("rider names are 3-24 letters, digits, '_' or '-'");
  if (store_.account(user)) return fail("that username is taken");
  if (store_.character_by_name(name)) return fail("that rider name is taken");
  PasswordCost cost;
  cost.kib = cfg_.argon2_kib;
  const std::string hash = hash_password(password, cost);
  if (hash.empty()) return fail("no randomness from the OS");
  std::int64_t aid = 0;
  if (!store_.create_account(user, hash, admin, &aid)) return fail(store_.last_error());
  CharacterRow row;
  row.account_id = aid;
  row.name = name;
  const ZoneDef& z = cfg_.zones.front();
  row.zone = z.geo.id;
  row.x = z.spawn_x;
  row.y = z.spawn_y;
  if (!store_.create_character(&row)) return fail(store_.last_error());
  ++stats_.creates;
  stats_.accounts = store_.account_count();
  stats_.characters = store_.character_count();
  log("account " + user + " created (rider " + name + (admin ? ", admin" : "") + ")");
  return true;
}

void MmoServer::on_login(PeerRt& p, const LoginMsg& m) {
  if (m.version != kMmoProtocolVersion) {
    login_fail(p, 8, "protocol " + std::to_string(m.version) + ", this Town Hall speaks " +
                         std::to_string(kMmoProtocolVersion));
    return;
  }
  if (shutdown_at_ >= 0.0) {
    login_fail(p, 7, "the Town Hall is closing", false);
    return;
  }
  std::optional<CharacterRow> row;
  std::optional<AccountRow> acc;
  bool resumed = false;
  if (m.mode == LoginMode::Resume) {
    const std::string digest = token_digest(m.token);
    const std::int64_t cid = m.token.size() == 64 ? store_.session_character(digest) : 0;
    if (cid == 0) {
      login_fail(p, 5, "that session is over; log in again");
      return;
    }
    store_.drop_session(digest);  // one use: a fresh token comes back
    auto live = chars_.find(cid);
    if (live != chars_.end()) {
      row = row_of(*live->second);
    } else {
      row = store_.character_by_id(cid);
    }
    if (row) acc = store_.account_by_id(row->account_id);
    resumed = true;
  } else {
    const std::string user = m.user;
    Lock& lock = locks_[lower(user)];
    if (lock.until > now_) {
      login_fail(p, 6, "too many tries; wait a little", false);
      return;
    }
    if (m.mode == LoginMode::Create) {
      if (!cfg_.open_registration) {
        login_fail(p, 7, "registration is closed here");
        return;
      }
      std::string err;
      if (!create_account(user, m.password, false, m.character, &err)) {
        login_fail(p, err.find("taken") != std::string::npos ? 3 : 8, err);
        return;
      }
    }
    acc = store_.account(user);
    if (!acc || !verify_password(m.password, acc->password)) {
      if (++lock.fails >= 5) {
        lock.until = now_ + 30.0;
        lock.fails = 0;
        log("login throttled for " + user);
      }
      login_fail(p, 1, "wrong username or password");
      return;
    }
    lock.fails = 0;
    row = store_.character_of(acc->id);
    if (!row) {
      CharacterRow r;
      r.account_id = acc->id;
      r.name = acc->username;
      r.zone = cfg_.zones.front().geo.id;
      r.x = cfg_.zones.front().spawn_x;
      r.y = cfg_.zones.front().spawn_y;
      if (!store_.create_character(&r)) {
        login_fail(p, 8, "could not make a rider: " + store_.last_error());
        return;
      }
      row = r;
    }
  }
  if (!row || !acc) {
    login_fail(p, 1, "no such rider");
    return;
  }
  if (acc->banned_until == -1 || acc->banned_until > unix_now()) {
    login_fail(p, 2, acc->banned_until == -1 ? "banned" : "banned for a while yet", false);
    transport_->disconnect(p.id, kMmoBanned);
    return;
  }
  const bool online = chars_.count(row->id) != 0;
  if (!online && stats_.online + stats_.linkdead >= cfg_.max_players) {
    login_fail(p, 4, "the Town Hall is full", false);
    return;
  }
  store_.touch_login(acc->id);
  attach(p, *row, *acc, resumed);
}

MmoServer::Character* MmoServer::attach(PeerRt& p, const CharacterRow& row, const AccountRow& acc, bool resumed) {
  auto it = chars_.find(row.id);
  Character* c = nullptr;
  bool took_over = false;
  if (it != chars_.end()) {
    c = it->second.get();
    if (c->peer != net::kNoPeer && c->peer != p.id) {
      // Logged in elsewhere: the old connection goes.
      auto op = peers_.find(c->peer);
      if (op != peers_.end()) op->second.character = 0;
      transport_->disconnect(c->peer, kMmoElsewhere);
      took_over = true;
    }
  } else {
    auto nc = std::make_unique<Character>();
    c = nc.get();
    c->id = row.id;
    c->entity = kCharacterBase | static_cast<std::uint64_t>(row.id);
    c->name = row.name;
    c->zone = cfg_.zone(row.zone) ? row.zone : cfg_.zones.front().geo.id;
    c->x = row.x;
    c->y = row.y;
    c->inventory = row.inventory;
    c->playtime = row.playtime;
    c->quests = quest_template_;
    json::Value st;
    std::string err;
    if (json::parse(row.story, &st, &err) && st.is_object()) {
      c->vars.from_json(st["vars"]);
      c->quests.from_json(st["quests"]);
    }
    for (const std::string& ch : cfg_.chat.channels) c->channels.insert(ch);
    chars_[row.id] = std::move(nc);
  }
  c->account = acc.id;
  c->account_name = acc.username;
  c->admin = acc.admin;
  c->muted_until = acc.muted_until;
  c->peer = p.id;
  c->linkdead_since = -1.0;
  c->pending.clear();
  c->known.clear();
  c->have_claim = false;
  c->ack = 0;  // a new connection counts its moves from 1
  c->claim_seq = 0;
  c->budget = 3.0f;
  c->zone_sent = false;
  p.character = row.id;
  // Fresh resume token.
  if (!c->token_digest.empty()) store_.drop_session(c->token_digest);
  const std::string token = new_token();
  c->token_digest = token_digest(token);
  store_.put_session(c->token_digest, c->id, unix_now() + 86400);
  LoginOkMsg ok;
  ok.entity = c->entity;
  ok.name = c->name;
  ok.token = token;
  ok.tick = stats_.ticks;
  ok.tick_rate = cfg_.tick_rate;
  ok.speed = cfg_.speed;
  ok.interest_radius = cfg_.interest_radius;
  ok.admin = c->admin;
  ok.resumed = resumed || it != chars_.end();
  send(p.id, net::Delivery::Reliable, encode(ok));
  send_zone(*c);
  send_inventory(*c);
  if (resumed) ++stats_.resumes;
  else ++stats_.logins;
  log(c->name + (ok.resumed ? " resumed" : " logged in") + (took_over ? " (took over the old connection)" : "") +
      " in " + c->zone);
  system_msg(*c, ok.resumed ? "Welcome back, " + c->name + "." : "Howdy, " + c->name + ". Say /help for commands.");
  return c;
}

void MmoServer::send_zone(Character& c) {
  const ZoneDef* z = cfg_.zone(c.zone);
  if (!z) return;
  ZoneMsg m;
  m.geo = z->geo;
  m.x = c.x;
  m.y = c.y;
  send(c.peer, net::Delivery::Reliable, encode(m));
  c.zone_sent = true;
  // Entering a zone = seeing nothing yet; replication fills it in.
  c.known.clear();
}

void MmoServer::send_inventory(Character& c) {
  InventoryMsg m;
  m.items = c.inventory;
  send(c.peer, net::Delivery::Reliable, encode(m));
}

void MmoServer::go_linkdead(Character& c) {
  c.peer = net::kNoPeer;
  c.linkdead_since = now_;
  c.pending.clear();
  c.dirty = true;
  store_.put_session(c.token_digest, c.id, unix_now() + static_cast<std::int64_t>(cfg_.linkdead_seconds) + 1);
  log(c.name + " dropped; link-dead for up to " + fmt1(cfg_.linkdead_seconds) + " s");
}

void MmoServer::logout(std::int64_t id) {
  auto it = chars_.find(id);
  if (it == chars_.end()) return;
  Character& c = *it->second;
  store_.save_characters({row_of(c)});
  store_.drop_sessions_of(c.id);
  ++stats_.saves;
  chars_.erase(it);
}

void MmoServer::disconnect_character(Character& c, std::uint32_t reason) {
  const net::PeerId peer = c.peer;
  const std::int64_t id = c.id;
  if (peer != net::kNoPeer) {
    auto p = peers_.find(peer);
    if (p != peers_.end()) p->second.character = 0;
    transport_->disconnect(peer, reason);
  }
  logout(id);  // kicked riders do not linger link-dead
}

CharacterRow MmoServer::row_of(const Character& c) const {
  CharacterRow r;
  r.id = c.id;
  r.account_id = c.account;
  r.name = c.name;
  r.zone = c.zone;
  r.x = c.x;
  r.y = c.y;
  r.inventory = c.inventory;
  r.playtime = c.playtime;
  json::Value st = json::Value::object();
  st.set("vars", c.vars.to_json());
  st.set("quests", c.quests.to_json());
  r.story = json::write(st);
  return r;
}

int MmoServer::save_all() {
  if (!store_.is_open()) return 0;
  const double t0 = wall_ms();
  std::vector<CharacterRow> rows;
  for (auto& [id, c] : chars_) {
    rows.push_back(row_of(*c));
    c->dirty = false;
    if (c->peer != net::kNoPeer && !c->token_digest.empty())
      store_.put_session(c->token_digest, c->id, unix_now() + 86400);
  }
  if (!store_.save_characters(rows)) log("save failed: " + store_.last_error());
  stats_.last_save_ms = wall_ms() - t0;
  stats_.saves += rows.size();
  return static_cast<int>(rows.size());
}

// ---------------------------------------------------------------------------
// Movement and anti-cheat

void MmoServer::strike(Character& c, float amount, const std::string& why) {
  c.strikes += amount;
  if (c.strikes >= 10.0f && c.kick == 0) {
    log(c.name + " kicked by the movement checks (" + why + ")");
    ++stats_.cheat_kicks;
    c.kick = kMmoCheat;
  }
}

void MmoServer::on_move(Character& c, const MoveMsg& m) {
  c.echo_ms = m.client_ms;
  c.rtt_ms = m.rtt_ms;
  for (const MoveCmd& cmd : m.cmds) {
    if (cmd.seq <= c.ack || c.pending.count(cmd.seq)) continue;
    if (cmd.seq > c.ack + 64) continue;  // nonsense from the future
    c.pending[cmd.seq] = cmd;
  }
  // Too many waiting = flooding (a speed hack sends faster than ticks).
  while (c.pending.size() > 12) {
    c.pending.erase(std::prev(c.pending.end()));
    ++c.dropped;
    ++stats_.dropped_moves;
  }
  if (!m.cmds.empty() && m.cmds.front().seq > c.claim_seq) {
    c.have_claim = true;
    c.claim_seq = m.cmds.front().seq;
    c.claim_x = m.claim_x;
    c.claim_y = m.claim_y;
  }
}

void MmoServer::move_riders(double dt) {
  std::vector<std::int64_t> kick;
  for (auto& [id, cp] : chars_) {
    Character& c = *cp;
    c.strikes = std::max(0.0f, c.strikes - static_cast<float>(dt) * 0.5f);
    c.chat_tokens = std::min(static_cast<float>(cfg_.chat.per_10s),
                             c.chat_tokens + static_cast<float>(dt) * cfg_.chat.per_10s / 10.0f);
    if (c.peer == net::kNoPeer) continue;
    c.playtime += dt;
    // One move per tick on average, a burst of 3 to ride out jitter.
    c.budget = std::min(3.0f, c.budget + 1.0f);
    const ZoneDef* z = cfg_.zone(c.zone);
    if (!z) continue;
    int applied = 0;
    while (!c.pending.empty() && c.budget >= 1.0f) {
      auto it = c.pending.begin();
      if (it->first != c.ack + 1 && applied > 0) break;
      const MoveCmd cmd = it->second;
      c.pending.erase(it);
      c.budget -= 1.0f;
      step_rider(z->geo, cfg_.speed, static_cast<float>(dt), cmd.mx, cmd.my, &c.x, &c.y);
      c.ack = cmd.seq;
      ++applied;
      c.dirty = true;
    }
    if (c.pending.size() > 8) {
      // More waiting than the budget allows for long: input flood.
      strike(c, 0.5f, "input flood");
    }
    if (c.have_claim && c.claim_seq == c.ack) {
      c.have_claim = false;
      const float dx = c.claim_x - c.x, dy = c.claim_y - c.y;
      const float err = std::sqrt(dx * dx + dy * dy);
      if (err > 2.0f) {
        c.corrected = true;
        ++c.corrections;
        ++stats_.corrections;
        const float teleport = cfg_.speed * 0.5f;
        strike(c, err > teleport ? 4.0f : 1.0f, err > teleport ? "teleport" : "position mismatch");
      }
    }
    if (const Portal* portal = portal_at(z->geo, c.x, c.y)) {
      change_zone(c, portal->to, portal->at_x, portal->at_y);
    }
  }
}

void MmoServer::change_zone(Character& c, const std::string& zone, float x, float y) {
  if (!cfg_.zone(zone)) return;
  log(c.name + " rode from " + c.zone + " to " + zone);
  c.zone = zone;
  c.x = x;
  c.y = y;
  c.pending.clear();
  c.have_claim = false;
  c.dirty = true;
  if (c.peer != net::kNoPeer) send_zone(c);
}

// ---------------------------------------------------------------------------
// Zones and replication

MmoServer::ZoneRt* MmoServer::zone_rt(const std::string& id) {
  for (auto& z : zones_)
    if (z->def->geo.id == id) return z.get();
  return nullptr;
}

void MmoServer::step_zones(double dt) {
  const int steps = std::max(1, static_cast<int>(std::lround(dt * runtime::kTickRate)));
  std::vector<runtime::NetActor> rows;
  for (auto& zp : zones_) {
    ZoneRt& z = *zp;
    z.ents.clear();
    z.riders = 0;
    if (z.world) {
      runtime::InputFrame in;
      for (int i = 0; i < steps; ++i) z.world->step(in);
      z.world->take_logs();
      rows.clear();
      z.world->net_capture(&rows);
      for (const runtime::NetActor& a : rows) {
        if (a.hidden || a.slot >= 0) continue;
        auto nm = z.actor_names.find(a.id);
        z.ents[a.id] = ZoneRt::Ent{EntityKind::Actor, nm != z.actor_names.end() ? nm->second : a.name, a.x, a.y};
      }
    }
    for (std::size_t i = 0; i < z.items.size(); ++i) {
      const auto& it = z.items[i];
      if (now_ < it.back_at) continue;
      z.ents[kItemBase | i] = ZoneRt::Ent{EntityKind::Item, it.spawn.name, it.spawn.x, it.spawn.y};
    }
  }
  for (auto& [id, c] : chars_) {
    ZoneRt* z = zone_rt(c->zone);
    if (!z) continue;
    z->ents[c->entity] = ZoneRt::Ent{EntityKind::Character, c->name, c->x, c->y};
    ++z->riders;
  }
  int total = 0;
  for (auto& zp : zones_) {
    zp->grid.reset(cfg_.interest_radius);
    for (const auto& [id, e] : zp->ents) zp->grid.insert(id, e.x, e.y);
    total += static_cast<int>(zp->ents.size());
  }
  stats_.world_entities = total;
}

void MmoServer::replicate() {
  const float r_in = cfg_.interest_radius;
  const float r_out = r_in * 1.15f;  // hysteresis: no flicker at the edge
  std::vector<std::uint64_t> near;
  long long visible_sum = 0;
  int online = 0, linkdead = 0, max_vis = 0;
  for (auto& [id, cp] : chars_) {
    Character& c = *cp;
    if (c.peer == net::kNoPeer) {
      ++linkdead;
      continue;
    }
    ++online;
    ZoneRt* z = zone_rt(c.zone);
    if (!z) continue;
    near.clear();
    z->grid.query(c.x, c.y, r_out, &near);
    std::unordered_map<std::uint64_t, bool> in_out;
    in_out.reserve(near.size());
    for (std::uint64_t e : near) in_out[e] = true;
    VanishMsg vanish;
    for (auto it = c.known.begin(); it != c.known.end();) {
      if (!in_out.count(it->first)) {
        vanish.ids.push_back(it->first);
        it = c.known.erase(it);
      } else {
        ++it;
      }
    }
    AppearMsg appear;
    SnapshotMsg snap;
    const bool full = now_ >= c.full_refresh;
    if (full) c.full_refresh = now_ + 1.0;
    for (std::uint64_t e : near) {
      if (e == c.entity) continue;
      const ZoneRt::Ent& ent = z->ents[e];
      auto k = c.known.find(e);
      if (k == c.known.end()) {
        const float dx = ent.x - c.x, dy = ent.y - c.y;
        if (dx * dx + dy * dy > r_in * r_in) continue;  // between radii: not yet
        appear.rows.push_back(AppearRow{e, ent.kind, ent.name, ent.x, ent.y});
        c.known[e] = {ent.x, ent.y};
        continue;
      }
      if (full || std::fabs(k->second.first - ent.x) > 0.01f || std::fabs(k->second.second - ent.y) > 0.01f) {
        snap.rows.push_back(SnapRow{e, ent.x, ent.y});
        k->second = {ent.x, ent.y};
      }
    }
    if (!vanish.ids.empty()) {
      stats_.vanishes += vanish.ids.size();
      send(c.peer, net::Delivery::Reliable, encode(vanish));
    }
    if (!appear.rows.empty()) {
      stats_.appears += appear.rows.size();
      send(c.peer, net::Delivery::Reliable, encode(appear));
    }
    snap.tick = stats_.ticks;
    snap.ack = c.ack;
    snap.x = c.x;
    snap.y = c.y;
    snap.corrected = c.corrected;
    snap.echo_ms = c.echo_ms;
    c.corrected = false;
    send(c.peer, net::Delivery::Unreliable, encode(snap));
    ++stats_.snapshots;
    const int vis = static_cast<int>(c.known.size());
    visible_sum += vis;
    max_vis = std::max(max_vis, vis);
  }
  stats_.online = online;
  stats_.linkdead = linkdead;
  stats_.avg_visible = online ? static_cast<double>(visible_sum) / online : 0.0;
  stats_.max_visible = max_vis;
}

// ---------------------------------------------------------------------------
// Interaction, quests

void MmoServer::apply_story_action(Character& c, const std::string& text) {
  story::Action a;
  if (!story::parse_action(text, &a, nullptr)) return;
  if (a.verb == "set" && a.args.size() >= 2) {
    c.vars.set(a.args[0], story::VarValue::parse(a.args[1]));
  } else if (a.verb == "add" && a.args.size() >= 2) {
    c.vars.add(a.args[0], std::atof(a.args[1].c_str()));
  } else if (a.verb == "give" && !a.args.empty()) {
    c.inventory[a.args[0]] += a.args.size() > 1 ? std::max(1, std::atoi(a.args[1].c_str())) : 1;
  } else if (a.verb == "quest" && a.args.size() >= 2) {
    if (a.args[0] == "start") c.quests.start(a.args[1]);
    else if (a.args[0] == "complete") c.quests.complete(a.args[1]);
    else if (a.args[0] == "fail") c.quests.fail(a.args[1]);
    else if (a.args[0] == "progress" && a.args.size() >= 3)
      c.quests.progress(a.args[1], a.args[2], a.args.size() > 3 ? std::atoi(a.args[3].c_str()) : 1);
  }
  c.dirty = true;
}

void MmoServer::on_interact(Character& c, const InteractMsg& m) {
  ZoneRt* z = zone_rt(c.zone);
  if (!z) return;
  if ((m.entity & kItemBase) != kItemBase || (m.entity & ~(kItemBase | 0xFFFFFFFFull)) != 0) {
    system_msg(c, "Nothing to do with that.");
    return;
  }
  const std::size_t i = static_cast<std::size_t>(m.entity & 0xFFFFFFFFull);
  if (i >= z->items.size() || now_ < z->items[i].back_at) {
    system_msg(c, "Somebody beat you to it.");
    return;
  }
  auto& it = z->items[i];
  const float dx = it.spawn.x - c.x, dy = it.spawn.y - c.y;
  if (dx * dx + dy * dy > 64.0f * 64.0f) {
    system_msg(c, "Too far to reach.");
    strike(c, 1.0f, "reach");
    return;
  }
  it.back_at = now_ + it.spawn.respawn;
  c.inventory[it.spawn.name] += 1;
  c.vars.add("items." + it.spawn.name, 1);
  // Any active quest with an objective of that name moves along.
  std::vector<std::string> done_before;
  for (const story::QuestDef* q : c.quests.defs()) {
    if (c.quests.status(q->id) != story::QuestStatus::Active) continue;
    for (const auto& o : q->objectives)
      if (o.id == it.spawn.name) c.quests.progress(q->id, o.id, 1);
  }
  for (const auto& ev : c.quests.take_events()) {
    if (ev.kind == story::QuestLog::Event::Kind::Completed) {
      system_msg(c, "Quest complete: " + ev.quest);
      if (const story::QuestDef* q = c.quests.def(ev.quest))
        for (const std::string& r : q->rewards) apply_story_action(c, r);
    }
  }
  c.dirty = true;
  ++stats_.pickups;
  send_inventory(c);
}

// ---------------------------------------------------------------------------
// Chat

void MmoServer::system_msg(Character& c, const std::string& text, const std::string& channel) {
  ChatMsg m;
  m.channel = channel;
  m.from = "Town Hall";
  m.text = text;
  send(c.peer, net::Delivery::Reliable, encode(m));
}

void MmoServer::deliver_chat(const std::string& channel, const std::string& from, const std::string& text,
                             const Character* origin, const std::string& to) {
  ChatMsg m;
  m.channel = channel;
  m.from = from;
  m.text = text;
  const std::vector<std::uint8_t> bytes = encode(m);
  const float r2 = cfg_.chat.say_radius * cfg_.chat.say_radius;
  for (auto& [id, cp] : chars_) {
    Character& c = *cp;
    if (c.peer == net::kNoPeer) continue;
    bool hear = false;
    if (channel == "world" || channel == "admin") hear = true;
    else if (channel == "zone") hear = origin && c.zone == origin->zone;
    else if (channel == "say") {
      hear = origin && c.zone == origin->zone &&
             (c.x - origin->x) * (c.x - origin->x) + (c.y - origin->y) * (c.y - origin->y) <= r2;
    } else if (channel == "whisper") hear = lower(c.name) == lower(to) || (origin && c.id == origin->id);
    else hear = c.channels.count(channel) != 0;
    if (hear) send(c.peer, net::Delivery::Reliable, bytes);
  }
  ChatLine line;
  line.time = now_;
  line.channel = channel;
  line.from = from;
  line.to = to;
  line.zone = origin ? origin->zone : std::string();
  line.text = text;
  chat_log_.push_back(std::move(line));
  while (chat_log_.size() > 500) chat_log_.pop_front();
  ++stats_.chat_lines;
}

MmoServer::Character* MmoServer::find_character(const std::string& name) {
  const std::string n = lower(name);
  for (auto& [id, c] : chars_)
    if (lower(c->name) == n) return c.get();
  return nullptr;
}

void MmoServer::on_chat(Character& c, const ChatSendMsg& m) {
  std::string text = clean_chat(m.text, std::max<std::size_t>(cfg_.chat.max_length, 400));
  if (text.empty()) return;
  if (text[0] == '/') {
    chat_command(c, text);
    return;
  }
  if (text.size() > cfg_.chat.max_length) text.resize(cfg_.chat.max_length);
  if (c.muted_until == -1 || c.muted_until > unix_now()) {
    system_msg(c, "You are muted.");
    return;
  }
  if (c.chat_tokens < 1.0f) {
    system_msg(c, "Easy there, partner: slow down.");
    return;
  }
  c.chat_tokens -= 1.0f;
  const std::string ch = lower(m.channel.empty() ? "say" : m.channel);
  if (ch == "admin" || ch == "system" || ch == "whisper") {
    system_msg(c, "You can't talk on " + ch + ".");
    return;
  }
  if (ch != "say" && ch != "zone" && ch != "world" && !c.channels.count(ch)) {
    system_msg(c, "You're not on " + ch + " (/join " + ch + ").");
    return;
  }
  deliver_chat(ch, c.name, text, &c);
}

void MmoServer::chat_command(Character& c, const std::string& text) {
  const std::vector<std::string> w = split_words(text);
  const std::string cmd = lower(w[0]);
  if (cmd == "/help") {
    system_msg(c, "/w <name> <text>, /join <channel>, /leave <channel>, /channels, /who, /where" +
                      std::string(c.admin ? ", /admin <command> (try /admin help)" : ""));
  } else if ((cmd == "/w" || cmd == "/whisper" || cmd == "/tell") && w.size() >= 3) {
    Character* t = find_character(w[1]);
    if (!t || t->peer == net::kNoPeer) {
      system_msg(c, w[1] + " isn't around.");
      return;
    }
    if (c.muted_until == -1 || c.muted_until > unix_now()) {
      system_msg(c, "You are muted.");
      return;
    }
    if (c.chat_tokens < 1.0f) {
      system_msg(c, "Easy there, partner: slow down.");
      return;
    }
    c.chat_tokens -= 1.0f;
    deliver_chat("whisper", c.name, clean_chat(rest_after(text, 2), cfg_.chat.max_length), &c, t->name);
  } else if (cmd == "/join" && w.size() >= 2) {
    const std::string ch = lower(w[1]);
    if (!valid_username(ch) || ch == "say" || ch == "zone" || ch == "world" || ch == "admin" ||
        ch == "system" || ch == "whisper") {
      system_msg(c, "Can't join " + w[1] + ".");
      return;
    }
    if (c.channels.size() >= 16) {
      system_msg(c, "That's plenty of channels.");
      return;
    }
    c.channels.insert(ch);
    system_msg(c, "Joined " + ch + ".");
  } else if (cmd == "/leave" && w.size() >= 2) {
    c.channels.erase(lower(w[1]));
    system_msg(c, "Left " + lower(w[1]) + ".");
  } else if (cmd == "/channels") {
    std::string s = "say, zone, world";
    for (const auto& ch : c.channels) s += ", " + ch;
    system_msg(c, s);
  } else if (cmd == "/who") {
    std::string s;
    int n = 0;
    for (auto& [id, o] : chars_)
      if (o->zone == c.zone && o->peer != net::kNoPeer) {
        if (n++ < 30) s += (s.empty() ? "" : ", ") + o->name;
      }
    system_msg(c, std::to_string(n) + " in " + c.zone + ": " + s);
  } else if (cmd == "/where") {
    system_msg(c, c.zone + " " + fmt1(c.x) + ", " + fmt1(c.y));
  } else if (cmd == "/admin" || cmd == "/a") {
    if (!c.admin) {
      system_msg(c, "Admins only.");
      return;
    }
    system_msg(c, admin(rest_after(text, 1), c.name), "admin");
  } else {
    system_msg(c, "Unknown command; /help lists them.");
  }
}

// ---------------------------------------------------------------------------
// Views

std::vector<CharacterView> MmoServer::characters() const {
  std::vector<CharacterView> out;
  for (const auto& [id, c] : chars_) {
    CharacterView v;
    v.entity = c->entity;
    v.id = c->id;
    v.name = c->name;
    v.account = c->account_name;
    v.zone = c->zone;
    v.x = c->x;
    v.y = c->y;
    v.online = c->peer != net::kNoPeer;
    v.admin = c->admin;
    v.rtt_ms = v.online && transport_ ? transport_->stats(c->peer).rtt_ms : 0;
    v.visible = static_cast<int>(c->known.size());
    v.strikes = c->strikes;
    v.corrections = c->corrections;
    for (const auto& [k, n] : c->inventory) v.items += n;
    out.push_back(std::move(v));
  }
  std::sort(out.begin(), out.end(), [](const CharacterView& a, const CharacterView& b) { return a.name < b.name; });
  return out;
}

std::vector<ZoneEntityView> MmoServer::zone_entities(const std::string& zone) const {
  std::vector<ZoneEntityView> out;
  for (const auto& z : zones_) {
    if (z->def->geo.id != zone) continue;
    for (const auto& [id, e] : z->ents) out.push_back(ZoneEntityView{id, e.kind, e.name, e.x, e.y});
  }
  return out;
}

std::vector<std::string> MmoServer::zone_ids() const {
  std::vector<std::string> out;
  for (const auto& z : cfg_.zones) out.push_back(z.geo.id);
  return out;
}

// ---------------------------------------------------------------------------
// Admin console

std::vector<std::string> MmoServer::admin_commands() const {
  std::vector<std::string> out;
  for (const auto& [k, v] : cmds_) out.push_back(k);
  return out;
}

std::string MmoServer::admin(const std::string& line, const std::string& by) {
  const std::vector<std::string> a = split_words(line);
  if (a.empty()) return {};
  auto it = cmds_.find(lower(a[0]));
  if (it == cmds_.end()) return "unknown command '" + a[0] + "' (try help)";
  if (lower(a[0]) != "help" && lower(a[0]) != "who" && lower(a[0]) != "stats" && lower(a[0]) != "zones")
    log("[admin] " + by + ": " + line);
  return it->second.fn(a, by);
}

void MmoServer::register_admin() {
  auto add = [this](const std::string& name, const std::string& help,
                    std::function<std::string(const std::vector<std::string>&, const std::string&)> fn) {
    cmds_[name] = Cmd{help, std::move(fn)};
  };
  auto need = [](const std::vector<std::string>& a, std::size_t n) { return a.size() >= n; };
  add("help", "help: this list", [this](const auto&, const auto&) {
    std::string s;
    for (const auto& [k, v] : cmds_) s += v.help + "\n";
    return s;
  });
  add("who", "who [zone]: riders in the world", [this](const auto& a, const auto&) {
    std::string s;
    int n = 0;
    for (const CharacterView& v : characters()) {
      if (a.size() > 1 && v.zone != a[1]) continue;
      ++n;
      if (n <= 50)
        s += v.name + " (" + v.account + ") " + v.zone + " " + fmt1(v.x) + "," + fmt1(v.y) +
             (v.online ? "" : " [link-dead]") + (v.admin ? " [admin]" : "") + "\n";
    }
    return std::to_string(n) + " rider(s)\n" + s;
  });
  add("stats", "stats: tick, bandwidth, interest", [this](const auto&, const auto&) {
    const MmoStats& s = stats_;
    std::ostringstream o;
    o << "tick " << s.ticks << " (" << fmt1(s.tick_ms_avg) << " ms avg, " << fmt1(s.tick_ms_max) << " max), "
      << s.online << " online, " << s.linkdead << " link-dead, " << s.accounts << " accounts\n"
      << fmt1(s.kbps_out) << " KB/s out, " << fmt1(s.kbps_in) << " KB/s in; sees " << fmt1(s.avg_visible)
      << " of " << s.world_entities << " entities on average (max " << s.max_visible << ")\n"
      << s.corrections << " corrections, " << s.cheat_kicks << " cheat kicks, " << s.logins << " logins, "
      << s.resumes << " resumes, " << s.failed_logins << " failed, " << s.chat_lines << " chat lines";
    return o.str();
  });
  add("zones", "zones: zones and who is in them", [this](const auto&, const auto&) {
    std::string s;
    for (const auto& z : zones_)
      s += z->def->geo.id + " \"" + z->def->geo.name + "\": " + std::to_string(z->riders) + " rider(s), " +
           std::to_string(z->ents.size()) + " entities" + (z->world ? ", World running" : "") + "\n";
    return s;
  });
  add("kick", "kick <name> [reason]", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 2)) return "kick <name>";
    Character* c = find_character(a[1]);
    if (!c) return a[1] + " isn't in the world";
    c->kick = kMmoKicked;  // next tick (it may be the admin's own rider)
    return "kicked " + c->name;
  });
  add("ban", "ban <user> [minutes] (no minutes = for good)", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 2)) return "ban <user> [minutes]";
    auto acc = store_.account(a[1]);
    if (!acc) return "no account " + a[1];
    const long long mins = a.size() > 2 ? std::atoll(a[2].c_str()) : 0;
    store_.set_banned(acc->id, mins > 0 ? unix_now() + mins * 60 : -1);
    for (auto& [id, c] : chars_)
      if (c->account == acc->id) c->kick = kMmoBanned;
    return "banned " + acc->username + (mins > 0 ? " for " + std::to_string(mins) + " min" : " for good");
  });
  add("unban", "unban <user>", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 2)) return "unban <user>";
    auto acc = store_.account(a[1]);
    if (!acc) return "no account " + a[1];
    store_.set_banned(acc->id, 0);
    return "unbanned " + acc->username;
  });
  add("mute", "mute <name> [minutes]", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 2)) return "mute <name> [minutes]";
    Character* c = find_character(a[1]);
    std::int64_t account = c ? c->account : 0;
    if (!c) {
      auto row = store_.character_by_name(a[1]);
      if (!row) return "no rider " + a[1];
      account = row->account_id;
    }
    const long long mins = a.size() > 2 ? std::atoll(a[2].c_str()) : 10;
    const std::int64_t until = mins > 0 ? unix_now() + mins * 60 : -1;
    store_.set_muted(account, until);
    if (c) {
      c->muted_until = until;
      system_msg(*c, "You've been muted.");
    }
    return "muted " + a[1];
  });
  add("unmute", "unmute <name>", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 2)) return "unmute <name>";
    Character* c = find_character(a[1]);
    if (c) {
      c->muted_until = 0;
      store_.set_muted(c->account, 0);
    } else if (auto row = store_.character_by_name(a[1])) {
      store_.set_muted(row->account_id, 0);
    } else {
      return "no rider " + a[1];
    }
    return "unmuted " + a[1];
  });
  add("tp", "tp <name> <x> <y> [zone]: move a rider", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 4)) return "tp <name> <x> <y> [zone]";
    Character* c = find_character(a[1]);
    if (!c) return a[1] + " isn't in the world";
    const float x = static_cast<float>(std::atof(a[2].c_str())), y = static_cast<float>(std::atof(a[3].c_str()));
    if (!std::isfinite(x) || !std::isfinite(y)) return "bad position";
    const std::string zone = a.size() > 4 ? a[4] : c->zone;
    if (!cfg_.zone(zone)) return "no zone " + zone;
    change_zone(*c, zone, x, y);
    return c->name + " now at " + zone + " " + fmt1(x) + "," + fmt1(y);
  });
  add("give", "give <name> <item> [n]", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 3)) return "give <name> <item> [n]";
    Character* c = find_character(a[1]);
    if (!c) return a[1] + " isn't in the world";
    const int n = a.size() > 3 ? std::max(1, std::atoi(a[3].c_str())) : 1;
    c->inventory[a[2]] += n;
    c->dirty = true;
    send_inventory(*c);
    return "gave " + c->name + " " + std::to_string(n) + " " + a[2];
  });
  add("take", "take <name> <item> [n]", [this, need](const auto& a, const auto&) -> std::string {
    if (!need(a, 3)) return "take <name> <item> [n]";
    Character* c = find_character(a[1]);
    if (!c) return a[1] + " isn't in the world";
    const int n = a.size() > 3 ? std::max(1, std::atoi(a[3].c_str())) : 1;
    int& have = c->inventory[a[2]];
    have = std::max(0, have - n);
    if (have == 0) c->inventory.erase(a[2]);
    c->dirty = true;
    send_inventory(*c);
    return "took " + a[2] + " from " + c->name;
  });
  add("announce", "announce <text>: to everyone", [this](const auto& a, const auto& by) -> std::string {
    if (a.size() < 2) return "announce <text>";
    std::string text;
    for (std::size_t i = 1; i < a.size(); ++i) text += (i > 1 ? " " : "") + a[i];
    deliver_chat("admin", by == "console" ? std::string("Town Hall") : by, text, nullptr);
    return "announced";
  });
  add("save", "save: write every rider to the database now", [this](const auto&, const auto&) {
    const int n = save_all();
    return "saved " + std::to_string(n) + " rider(s) in " + fmt1(stats_.last_save_ms) + " ms";
  });
  add("account", "account create <user> <pass> [admin] | account admin <user> on|off | account password <user> <pass>",
      [this, need](const auto& a, const auto&) -> std::string {
        if (need(a, 4) && a[1] == "create") {
          std::string err;
          if (!create_account(a[2], a[3], a.size() > 4 && a[4] == "admin", {}, &err)) return err;
          return "account " + a[2] + " created";
        }
        if (need(a, 4) && a[1] == "admin") {
          auto acc = store_.account(a[2]);
          if (!acc) return "no account " + a[2];
          const bool on = a[3] == "on";
          store_.set_admin(acc->id, on);
          for (auto& [id, c] : chars_)
            if (c->account == acc->id) c->admin = on;
          return acc->username + (on ? " is an admin" : " is not an admin");
        }
        if (need(a, 4) && a[1] == "password") {
          auto acc = store_.account(a[2]);
          if (!acc) return "no account " + a[2];
          if (a[3].size() < 6) return "passwords are 6-128 characters";
          PasswordCost cost;
          cost.kib = cfg_.argon2_kib;
          store_.set_password(acc->id, hash_password(a[3], cost));
          return "password changed for " + acc->username;
        }
        return "account create <user> <pass> [admin] | account admin <user> on|off | account password <user> <pass>";
      });
  add("quest", "quest <name> start|progress|complete|fail <quest> [objective]",
      [this, need](const auto& a, const auto&) -> std::string {
        if (!need(a, 4)) return "quest <name> start|progress|complete|fail <quest> [objective]";
        Character* c = find_character(a[1]);
        if (!c) return a[1] + " isn't in the world";
        std::string action = "quest " + a[2] + " " + a[3] + (a.size() > 4 ? " " + a[4] : "");
        apply_story_action(*c, action);
        return c->name + ": " + a[3] + " is " + story::quest_status_name(c->quests.status(a[3]));
      });
  add("var", "var <name> <key> [value]: read / set a rider's story variable",
      [this, need](const auto& a, const auto&) -> std::string {
        if (!need(a, 3)) return "var <name> <key> [value]";
        Character* c = find_character(a[1]);
        if (!c) return a[1] + " isn't in the world";
        if (a.size() > 3) {
          c->vars.set(a[2], story::VarValue::parse(a[3]));
          c->dirty = true;
        }
        return a[2] + " = " + c->vars.get(a[2]).str();
      });
  add("strongbox", "strongbox export|import <name> <slot>: a rider <-> a Strongbox save slot",
      [this, need](const auto& a, const auto&) -> std::string {
        if (!need(a, 4)) return "strongbox export|import <name> <slot>";
        std::string err;
        if (a[1] == "export") return export_to_strongbox(a[2], a[3], &err) ? "exported " + a[2] + " to slot " + a[3] : err;
        if (a[1] == "import") return import_from_strongbox(a[2], a[3], &err) ? "imported slot " + a[3] + " into " + a[2] : err;
        return "strongbox export|import <name> <slot>";
      });
  add("shutdown", "shutdown [seconds]: warn, save and close", [this](const auto& a, const auto& by) {
    const double secs = a.size() > 1 ? std::max(0.0, std::atof(a[1].c_str())) : 0.0;
    shutdown_at_ = now_ + secs;
    deliver_chat("admin", by == "console" ? std::string("Town Hall") : by,
                 "The Town Hall closes in " + fmt1(secs) + " s.", nullptr);
    return "closing in " + fmt1(secs) + " s";
  });
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
