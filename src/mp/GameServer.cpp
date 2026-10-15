#include "mp/GameServer.h"
#include "profiler/Profiler.h"

#include "core/JsonMini.h"
#include "net/Net.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace ts {
namespace tombstone {
namespace mp {

namespace fs = std::filesystem;
using net::Delivery;
using net::TransportEvent;

namespace {

// Inputs a rider may bank before the oldest are ridden off in one tick
// (keeps a burst after a lag spike from turning into lasting input delay).
constexpr std::size_t kMaxBacklog = 3;
constexpr std::size_t kMaxPending = 64;
constexpr std::size_t kMaxLogsPerTick = 64;

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string folder_project_name(const std::string& dir) {
  const std::string text = read_text(fs::path(dir) / "project.json");
  if (auto obj = json_mini::parse_object(text)) {
    const std::string name = json_mini::get_string(*obj, "name", "");
    if (!name.empty()) return name;
  }
  const fs::path p = fs::path(dir).lexically_normal();
  const std::string leaf =
      (p.has_filename() ? p.filename() : p.parent_path().filename()).string();
  return leaf.empty() ? std::string("TombStone") : leaf;
}

std::string slot_tag(int slot) { return "P" + std::to_string(slot + 1); }

}  // namespace

GameServer::GameServer() = default;
GameServer::~GameServer() { stop(); }

bool GameServer::start(ServerConfig config, std::string* error_out) {
  stop();
  auto fail = [&](const std::string& why) {
    if (error_out) *error_out = why;
    world_.clear();
    transport_.reset();
    sim_ = nullptr;
    return false;
  };
  config_ = std::move(config);
  config_.max_players = std::clamp(config_.max_players, 1, kMaxPosse);
  config_.snapshot_every = std::clamp(config_.snapshot_every, 1, 30);

  scene_json::SceneDoc doc;
  std::string err;
  if (config_.scene) {
    doc = *config_.scene;
    scene_json_ = scene_json::write(doc);
  } else {
    const std::string path = scene_json::scene_path_for_project(config_.project_dir);
    scene_json_ = read_text(path);
    if (scene_json_.empty()) return fail("no scene.json at " + path);
    if (!scene_json::parse(scene_json_, &doc, &err, path)) return fail(err);
  }
  if (scene_json_.size() > kMaxSceneBytes) return fail("scene.json is too big to send");
  scene_hash_ = scene_hash(scene_json_);
  world_.set_net_role(runtime::NetRole::Server);
  if (!world_.build(doc.entities, doc.tile_solidity, config_.project_dir, &err)) {
    return fail(err);
  }
  if (world_.player_count() == 0) {
    return fail("the scene has no rider (add a Player component) to seat the posse on");
  }
  project_name_ = !config_.project_name.empty() ? config_.project_name
                  : config_.project_dir.empty() ? std::string("TombStone")
                                                : folder_project_name(config_.project_dir);
  // Empty seats ride hidden until somebody takes them.
  slot_rider_.fill(0);
  for (int s = 0; s < kMaxPosse; ++s) {
    if (const runtime::Actor* a = world_.player(s)) {
      slot_rider_[static_cast<std::size_t>(s)] = a->data.id;
      seat_visible(s, a->data.id, false);
    }
  }

  std::unique_ptr<net::Transport> inner =
      config_.transport ? std::move(config_.transport) : net::make_enet_transport();
  auto sim = std::make_unique<net::SimTransport>(std::move(inner), config_.sim, 0xC0FFEEu);
  sim_ = sim.get();
  transport_ = std::move(sim);
  // Room for a few would-be riders past the posse so they hear "full".
  if (!transport_->listen(config_.port,
                          static_cast<std::size_t>(config_.max_players) + 4, &err)) {
    return fail(err.empty() ? "could not listen" : err);
  }
  running_ = true;
  accumulator_ = 0.0;
  stats_ = ServerStats{};
  log("Server open on UDP " + std::to_string(port()) + ": \"" + project_name_ + "\", " +
      std::to_string(world_.actors().size()) + " entities, up to " +
      std::to_string(config_.max_players) + " riders");
  return true;
}

void GameServer::stop() {
  if (!transport_) {
    running_ = false;
    return;
  }
  for (auto& [id, p] : peers_) {
    transport_->disconnect(id, kReasonShutdown);
  }
  // Let the goodbyes out (up to a third of a second) so riders hear the
  // reason instead of timing out.
  if (!peers_.empty()) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    std::size_t gone = 0;
    net::TransportEvent ev;
    while (gone < peers_.size() && std::chrono::steady_clock::now() < until) {
      transport_->flush();
      while (transport_->poll(&ev)) {
        gone += ev.type == net::TransportEvent::Type::Disconnected ? 1 : 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  transport_->flush();
  transport_->close();
  transport_.reset();
  sim_ = nullptr;
  peers_.clear();
  world_.clear();
  running_ = false;
}

std::uint16_t GameServer::port() const { return transport_ ? transport_->port() : 0; }

void GameServer::set_link_sim(const net::LinkSim& sim) {
  config_.sim = sim;
  if (sim_) sim_->set_sim(sim);
}

net::LinkSim GameServer::link_sim() const { return sim_ ? sim_->sim() : config_.sim; }

void GameServer::log(std::string line) {
  if (log_.size() < 1024) log_.push_back(std::move(line));
}

std::vector<std::string> GameServer::take_log() {
  std::vector<std::string> out;
  out.swap(log_);
  return out;
}

std::vector<runtime::LogEntry> GameServer::take_world_logs() {
  std::vector<runtime::LogEntry> out;
  out.swap(world_logs_);
  return out;
}

GameServer::Peer* GameServer::peer(net::PeerId id) {
  const auto it = peers_.find(id);
  return it == peers_.end() ? nullptr : &it->second;
}

int GameServer::rider_count() const {
  int n = 0;
  for (const auto& [id, p] : peers_) n += p.seated ? 1 : 0;
  return n;
}

std::vector<ServerRider> GameServer::riders() const {
  std::vector<ServerRider> out;
  for (const auto& [id, p] : peers_) {
    if (!p.seated) continue;
    ServerRider r;
    r.slot = p.slot;
    r.name = p.name;
    r.rider = p.rider;
    r.rtt_ms = p.rtt_ms;
    r.last_seq = p.last_seq;
    r.queued = p.pending.size();
    r.joined_tick = p.joined_tick;
    r.link = transport_ ? transport_->stats(id) : net::LinkStats{};
    r.kbps_out = p.kbps_out;
    r.kbps_in = p.kbps_in;
    out.push_back(std::move(r));
  }
  std::sort(out.begin(), out.end(),
            [](const ServerRider& a, const ServerRider& b) { return a.slot < b.slot; });
  return out;
}

void GameServer::seat_visible(int slot, std::uint64_t rider, bool on) {
  (void)slot;
  world_.script_set_visible(rider, on);
  world_.script_set_collider(rider, on);
}

// --- Network in ---------------------------------------------------------------

void GameServer::poll() {
  if (!transport_) return;
  TransportEvent ev;
  int budget = 4096;
  while (budget-- > 0 && transport_->poll(&ev)) {
    handle(ev);
  }
}

void GameServer::handle(const TransportEvent& ev) {
  switch (ev.type) {
    case TransportEvent::Type::Connected: {
      Peer p;
      p.id = ev.peer;
      peers_[ev.peer] = std::move(p);
      break;
    }
    case TransportEvent::Type::Disconnected: {
      if (Peer* p = peer(ev.peer)) {
        unseat(*p, ev.reason);
        peers_.erase(ev.peer);
      }
      break;
    }
    case TransportEvent::Type::Received: {
      Peer* p = peer(ev.peer);
      MsgType type;
      if (!p || !peek_type(ev.data, &type)) break;
      if (type == MsgType::Hello) {
        HelloMsg hello;
        if (!p->seated && decode(ev.data, &hello)) on_hello(*p, hello);
      } else if (type == MsgType::Input) {
        InputMsg in;
        if (p->seated && decode(ev.data, &in)) on_input(*p, in);
      } else if (type == MsgType::Plugin) {
        // Plugin channels: the world's owner decides what a rider may ask.
        PluginMsg pm;
        if (p->seated && decode(ev.data, &pm)) world_.net_deliver(pm.channel, p->slot, pm.bytes);
      }
      break;
    }
  }
}

void GameServer::on_hello(Peer& p, const HelloMsg& hello) {
  auto reject = [&](std::uint32_t reason, const std::string& text) {
    RejectMsg r;
    r.reason = reason;
    r.text = text;
    transport_->send(p.id, Delivery::Reliable, encode(r));
    transport_->disconnect(p.id, reason);
    ++stats_.rejected;
    log("Turned away " + clean_name(hello.name) + ": " + text);
  };
  if (hello.version != kProtocolVersion) {
    reject(kReasonVersion, "server speaks protocol " + std::to_string(kProtocolVersion) +
                               ", client " + std::to_string(hello.version));
    return;
  }
  int slot = -1;
  if (rider_count() < config_.max_players) {
    for (int s = 0; s < config_.max_players && slot < 0; ++s) {
      bool taken = false;
      for (const auto& [id, o] : peers_) taken |= o.seated && o.slot == s;
      if (!taken) slot = s;
    }
  }
  if (slot < 0) {
    reject(kReasonFull, "The posse is full: four riders already. Try again later.");
    return;
  }
  std::uint64_t rider = slot_rider_[static_cast<std::size_t>(slot)];
  if (rider == 0 || !world_.find(rider)) {
    rider = world_.ensure_rider(slot);
    slot_rider_[static_cast<std::size_t>(slot)] = rider;
  }
  if (rider == 0) {
    reject(kReasonKicked, "no rider entity to seat you on");
    return;
  }
  seat_visible(slot, rider, true);
  p.seated = true;
  p.slot = slot;
  p.name = clean_name(hello.name);
  p.rider = rider;
  p.joined_tick = world_.tick();
  p.last_seq = 0;
  p.pending.clear();
  p.last = runtime::PlayerInput{};

  WelcomeMsg w;
  w.slot = slot;
  w.rider = rider;
  w.tick = world_.tick();
  w.snapshot_every = config_.snapshot_every;
  w.project_name = project_name_;
  w.scene_json = scene_json_;
  w.scene_hash = scene_hash_;
  transport_->send(p.id, Delivery::Reliable, encode(w));

  ++stats_.joins;
  const std::string line = p.name + " rode in as " + slot_tag(slot) +
                           (hello.scene_hash != 0 && hello.scene_hash != scene_hash_
                                ? " (their scene.json differs; riding ours)"
                                : "");
  log(line);
  EventMsg e;
  e.kind = EventMsg::Kind::Join;
  e.slot = slot;
  e.channel = "posse";
  e.text = p.name + " rode in as " + slot_tag(slot);
  broadcast(Delivery::Reliable, encode(e));
  send_roster();
}

void GameServer::on_input(Peer& p, const InputMsg& in) {
  p.rtt_ms = in.rtt_ms;
  p.echo_time_ms = in.client_time_ms;
  for (const InputCmd& c : in.cmds) {
    if (c.seq <= p.last_seq || p.pending.count(c.seq)) continue;
    if (p.pending.size() >= kMaxPending) break;
    p.pending.emplace(c.seq, c.input);
  }
}

void GameServer::unseat(Peer& p, std::uint32_t reason) {
  if (!p.seated) return;
  p.seated = false;
  seat_visible(p.slot, p.rider, false);
  ++stats_.leaves;
  const std::string text = p.name + " (" + slot_tag(p.slot) + ") rode out: " +
                           reason_text(reason);
  log(text);
  EventMsg e;
  e.kind = EventMsg::Kind::Leave;
  e.slot = p.slot;
  e.channel = "posse";
  e.text = text;
  const net::PeerId gone = p.id;
  for (auto& [id, o] : peers_) {
    if (o.seated && id != gone) transport_->send(id, Delivery::Reliable, encode(e));
  }
  RosterMsg r;
  for (const auto& [id, o] : peers_) {
    if (o.seated && id != gone) r.riders.push_back({o.slot, o.name, o.rtt_ms, o.rider});
  }
  const auto msg = encode(r);
  for (auto& [id, o] : peers_) {
    if (o.seated && id != gone) transport_->send(id, Delivery::Reliable, msg);
  }
}

// --- Tick -----------------------------------------------------------------------

runtime::InputFrame GameServer::gather_input() {
  runtime::InputFrame frame;
  for (auto& [id, p] : peers_) {
    if (!p.seated) continue;
    runtime::PlayerInput in;
    if (!p.pending.empty()) {
      // Every input is applied exactly once, in order, so the client's
      // prediction (which did the same) lands where we do. A backlog after
      // a lag spike is ridden off now: the extra moves go through the same
      // movement code prediction uses, their buttons ride on this tick.
      std::uint32_t folded = 0;
      while (p.pending.size() > kMaxBacklog) {
        auto old = p.pending.begin();
        world_.predict_rider(p.rider, old->second);
        folded |= old->second.buttons;
        p.last_seq = old->first;
        p.pending.erase(old);
      }
      auto it = p.pending.begin();
      in = it->second;
      in.buttons |= folded;
      p.last_seq = it->first;
      p.pending.erase(it);
      p.last = in;
    }
    // Nothing arrived in time: the rider waits for it (no guessing, so no
    // correction later).
    in.connected = true;
    frame.slot(p.slot) = in;
  }
  return frame;
}

void GameServer::tick_once() {
  if (!running_) return;
  poll();
  world_.step(gather_input());
  ++stats_.ticks;
  forward_world_output();
  forward_plugin_posts();
  if (world_.tick() % static_cast<std::uint64_t>(config_.snapshot_every) == 0) {
    send_snapshots();
  }
  if (world_.tick() % runtime::kTickRate == 0) {
    send_roster();
  }
  transport_->flush();
}

int GameServer::update(double real_seconds) {
  if (!running_) return 0;
  poll();
  real_seconds = std::clamp(real_seconds, 0.0, 0.25);
  accumulator_ += real_seconds;
  int n = 0;
  while (accumulator_ >= runtime::kTickSeconds && n < 8) {
    accumulator_ -= runtime::kTickSeconds;
    tick_once();
    ++n;
  }
  if (n == 8) accumulator_ = 0.0;
  transport_->flush();
  measure(real_seconds);
  return n;
}

void GameServer::measure(double real_seconds) {
  measure_window_ += real_seconds;
  if (profiler::Profiler::get().enabled()) {
    profiler::Profiler& pr = profiler::Profiler::get();
    double rtt = 0.0;
    int n = 0;
    for (const auto& [id, peer] : peers_) {
      if (!peer.seated) continue;
      rtt += peer.rtt_ms;
      ++n;
    }
    pr.counter("net.server.riders", static_cast<double>(n));
    pr.counter("net.server.rtt_ms", n ? rtt / n : 0.0);
    pr.counter("net.server.kbps_out", stats_.kbps_out);
    pr.counter("net.server.kbps_in", stats_.kbps_in);
    pr.counter("net.server.snapshot_hz", stats_.snapshot_hz);
    pr.counter("net.server.snapshot_bytes", static_cast<double>(stats_.last_snapshot_bytes));
  }
  if (measure_window_ < 1.0) return;
  double out = 0.0;
  double in = 0.0;
  int seated = 0;
  for (auto& [id, p] : peers_) {
    const net::LinkStats s = transport_->stats(id);
    p.kbps_out = static_cast<double>(s.bytes_sent - std::min(s.bytes_sent, p.bytes_out_mark)) /
                 1024.0 / measure_window_;
    p.kbps_in = static_cast<double>(s.bytes_received - std::min(s.bytes_received, p.bytes_in_mark)) /
                1024.0 / measure_window_;
    p.bytes_out_mark = s.bytes_sent;
    p.bytes_in_mark = s.bytes_received;
    out += p.kbps_out;
    in += p.kbps_in;
    seated += p.seated ? 1 : 0;
  }
  stats_.kbps_out = out;
  stats_.kbps_in = in;
  stats_.snapshot_hz = seated > 0 ? static_cast<double>(stats_.snapshots_sent - measure_snapshots_) /
                                        seated / measure_window_
                                  : 0.0;
  measure_snapshots_ = stats_.snapshots_sent;
  measure_window_ = 0.0;
}

void GameServer::broadcast(Delivery delivery, const std::vector<std::uint8_t>& msg) {
  for (auto& [id, p] : peers_) {
    if (p.seated) transport_->send(id, delivery, msg);
  }
}

void GameServer::send_snapshots() {
  world_.net_capture(&rows_);
  SnapshotMsg snap;
  snap.tick = world_.tick();
  snap.actors = rows_;
  for (auto& [id, p] : peers_) {
    if (!p.seated) continue;
    snap.ack_seq = p.last_seq;
    snap.echo_time_ms = p.echo_time_ms;
    const auto bytes = encode(snap);
    stats_.last_snapshot_bytes = bytes.size();
    transport_->send(id, Delivery::Unreliable, bytes);
    ++stats_.snapshots_sent;
  }
}

void GameServer::send_roster() {
  RosterMsg r;
  for (const auto& [id, p] : peers_) {
    if (p.seated) r.riders.push_back({p.slot, p.name, p.rtt_ms, p.rider});
  }
  std::sort(r.riders.begin(), r.riders.end(),
            [](const RosterEntry& a, const RosterEntry& b) { return a.slot < b.slot; });
  broadcast(Delivery::Reliable, encode(r));
}

void GameServer::forward_plugin_posts() {
  for (runtime::NetPost& post : world_.take_net_posts()) {
    PluginMsg m;
    m.channel = std::move(post.channel);
    m.bytes = std::move(post.bytes);
    const std::vector<std::uint8_t> msg = encode(m);
    if (post.to_slot < 0) {
      broadcast(Delivery::Reliable, msg);
      continue;
    }
    for (const auto& [id, p] : peers_) {
      if (p.seated && p.slot == post.to_slot) transport_->send(id, Delivery::Reliable, msg);
    }
  }
}

void GameServer::forward_world_output() {
  // Telegraph lines (script logs, toasts, trigger events) go to everyone.
  std::vector<runtime::LogEntry> logs = world_.take_logs();
  std::size_t sent = 0;
  for (runtime::LogEntry& e : logs) {
    if (sent++ < kMaxLogsPerTick) {
      EventMsg m;
      m.kind = EventMsg::Kind::Log;
      m.level = static_cast<std::uint8_t>(e.level);
      m.channel = e.channel;
      m.text = runtime::format_log(e, true);
      broadcast(Delivery::Reliable, encode(m));
    }
    if (world_logs_.size() < runtime::World::kMaxPendingLogs) world_logs_.push_back(std::move(e));
  }
  // New toasts go out once, as events (the client keeps its own timer).
  std::vector<std::string> live;
  for (const runtime::Toast& t : world_.toasts()) {
    const std::string key = std::to_string(t.entity) + "|" + std::to_string(t.until) + "|" +
                            std::to_string(t.slot) + "|" + t.text;
    live.push_back(key);
    if (std::find(toasts_sent_.begin(), toasts_sent_.end(), key) != toasts_sent_.end()) continue;
    EventMsg m;
    m.kind = EventMsg::Kind::Toast;
    m.slot = t.slot;
    m.channel = "toast";
    m.text = t.text;
    m.seconds = static_cast<float>(static_cast<double>(t.until + 1 - world_.tick()) *
                                   runtime::kTickSeconds);
    broadcast(Delivery::Reliable, encode(m));
  }
  toasts_sent_.swap(live);
}

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
