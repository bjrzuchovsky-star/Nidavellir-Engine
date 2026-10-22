#include "mp/GameClient.h"
#include "profiler/Profiler.h"

#include "scene/SceneJson.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace ts {
namespace tombstone {
namespace mp {

using net::Delivery;
using net::TransportEvent;

namespace {

constexpr std::size_t kMaxUnacked = 120;   // 2 s of input
constexpr std::size_t kMaxSnaps = 64;
constexpr float kSnapPx = 64.0f;           // bigger corrections jump
constexpr float kSmoothDecay = 0.80f;      // per tick

double wall_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string read_text(const std::string& path) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

const char* to_string(ClientState state) {
  switch (state) {
    case ClientState::Idle: return "idle";
    case ClientState::Connecting: return "riding in";
    case ClientState::Joined: return "in the posse";
    case ClientState::Rejected: return "turned away";
    case ClientState::Disconnected: return "rode off";
  }
  return "?";
}

GameClient::GameClient() : start_wall_(wall_seconds()) {}
GameClient::~GameClient() { disconnect(); }

std::uint32_t GameClient::now_ms() const {
  return static_cast<std::uint32_t>((wall_seconds() - start_wall_) * 1000.0) + 1u;
}

bool GameClient::connect(ClientConfig config, std::string* error_out) {
  disconnect();
  config_ = std::move(config);
  config_.name = clean_name(config_.name);
  world_.set_net_role(runtime::NetRole::Client);
  config_.interp_ms = std::clamp(config_.interp_ms, 0.0, 1000.0);
  std::unique_ptr<net::Transport> inner =
      config_.transport ? std::move(config_.transport) : net::make_enet_transport();
  auto sim = std::make_unique<net::SimTransport>(
      std::move(inner), config_.sim,
      0x5EED0000u ^ static_cast<std::uint64_t>(std::hash<std::string>{}(config_.name)));
  sim_ = sim.get();
  transport_ = std::move(sim);
  std::string err;
  server_ = transport_->connect(config_.host, config_.port, &err);
  if (server_ == net::kNoPeer) {
    if (error_out) *error_out = err.empty() ? "could not start connecting" : err;
    transport_.reset();
    sim_ = nullptr;
    state_ = ClientState::Disconnected;
    status_ = err;
    return false;
  }
  state_ = ClientState::Connecting;
  status_.clear();
  world_.clear();
  world_ready_ = false;
  slot_ = -1;
  rider_ = 0;
  clock_ = 0.0;
  accumulator_ = 0.0;
  seq_ = 0;
  acked_ = 0;
  unacked_.clear();
  snaps_.clear();
  have_pred_ = false;
  have_offset_ = false;
  err_x_ = err_y_ = 0.0f;
  roster_ = RosterMsg{};
  toasts_.clear();
  stats_ = ClientStats{};
  stats_.interp_ms = config_.interp_ms;
  bytes_in_mark_ = bytes_out_mark_ = 0;
  measure_window_ = 0.0;
  measure_snaps_ = 0;
  return true;
}

void GameClient::disconnect(const std::function<void()>& pump) {
  if (!transport_) return;
  if (server_ != net::kNoPeer &&
      (state_ == ClientState::Joined || state_ == ClientState::Connecting)) {
    transport_->disconnect(server_, kReasonQuit);
    // Wait on the condition (the server's acknowledgement), not a fixed
    // time: the goodbye crosses the lag sim, and ENet only sends it once
    // every queued reliable packet is acked. Bounded so a dead server can't
    // hold us; then the server frees the seat on its own peer timeout.
    const net::LinkSim sim = link_sim();
    const double wait =
        std::max(2.0, 4.0 * (sim.latency_ms + sim.jitter_ms) / 1000.0 + 0.5);
    const double until = wall_seconds() + wait;
    TransportEvent ev;
    bool acknowledged = false;
    while (!acknowledged && wall_seconds() < until) {
      transport_->flush();
      while (transport_->poll(&ev)) {
        acknowledged |= ev.type == TransportEvent::Type::Disconnected;
      }
      if (pump) pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    transport_->flush();
  }
  transport_->close();
  transport_.reset();
  sim_ = nullptr;
  if (state_ == ClientState::Joined || state_ == ClientState::Connecting) {
    state_ = ClientState::Disconnected;
    status_ = reason_text(kReasonQuit);
  }
}

void GameClient::vanish() {
  if (!transport_) return;
  transport_->close();
  transport_.reset();
  sim_ = nullptr;
  state_ = ClientState::Disconnected;
  status_ = "vanished";
}

void GameClient::set_link_sim(const net::LinkSim& sim) {
  config_.sim = sim;
  if (sim_) sim_->set_sim(sim);
}

net::LinkSim GameClient::link_sim() const { return sim_ ? sim_->sim() : config_.sim; }

void GameClient::event(std::string line) {
  if (events_.size() < 1024) events_.push_back(std::move(line));
}

std::vector<std::string> GameClient::take_events() {
  std::vector<std::string> out;
  out.swap(events_);
  return out;
}

std::vector<ClientToast> GameClient::toasts() const {
  std::vector<ClientToast> out;
  for (const ClientToast& t : toasts_) {
    if (t.until >= clock_) out.push_back(t);
  }
  return out;
}

// --- Network in -------------------------------------------------------------------

void GameClient::poll() {
  if (!transport_) return;
  TransportEvent ev;
  int budget = 4096;
  while (budget-- > 0 && transport_ && transport_->poll(&ev)) {
    handle(ev);
  }
}

void GameClient::handle(const TransportEvent& ev) {
  switch (ev.type) {
    case TransportEvent::Type::Connected: {
      HelloMsg h;
      h.name = config_.name;
      if (!config_.project_dir.empty()) {
        const std::string text =
            read_text(scene_json::scene_path_for_project(config_.project_dir));
        h.scene_hash = text.empty() ? 0 : scene_hash(text);
      }
      transport_->send(server_, Delivery::Reliable, encode(h));
      transport_->flush();
      break;
    }
    case TransportEvent::Type::Disconnected: {
      if (state_ != ClientState::Rejected) {
        state_ = ClientState::Disconnected;
        status_ = reason_text(ev.reason);
      }
      event("Lost the server: " + status_);
      break;
    }
    case TransportEvent::Type::Received: {
      MsgType type;
      if (!peek_type(ev.data, &type)) break;
      switch (type) {
        case MsgType::Welcome: {
          WelcomeMsg w;
          if (state_ == ClientState::Connecting && decode(ev.data, &w)) on_welcome(w);
          break;
        }
        case MsgType::Reject: {
          RejectMsg r;
          if (decode(ev.data, &r)) {
            state_ = ClientState::Rejected;
            status_ = r.text.empty() ? reason_text(r.reason) : r.text;
            event("Turned away: " + status_);
          }
          break;
        }
        case MsgType::Snapshot: {
          SnapshotMsg s;
          if (joined() && decode(ev.data, &s)) on_snapshot(std::move(s));
          break;
        }
        case MsgType::Event: {
          EventMsg e;
          if (!decode(ev.data, &e)) break;
          if (e.kind == EventMsg::Kind::Toast) {
            if (e.slot < 0 || e.slot == slot_) {
              toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                           [&](const ClientToast& t) { return t.until < clock_; }),
                            toasts_.end());
              if (toasts_.size() < 32) {
                toasts_.push_back({e.slot, e.text, clock_ + std::max(0.1f, e.seconds)});
              }
            }
          } else {
            event(e.text);
          }
          break;
        }
        case MsgType::Roster: {
          RosterMsg r;
          if (decode(ev.data, &r)) roster_ = std::move(r);
          break;
        }
        case MsgType::Plugin: {
          PluginMsg m;
          if (joined() && world_ready_ && decode(ev.data, &m)) {
            world_.net_deliver(m.channel, -1, m.bytes);
          }
          break;
        }
        default:
          break;
      }
      break;
    }
  }
}

void GameClient::on_welcome(const WelcomeMsg& w) {
  scene_json::SceneDoc doc;
  std::string err;
  if (!scene_json::parse(w.scene_json, &doc, &err, "server scene.json") ||
      !world_.build(doc.entities, doc.tile_solidity, config_.project_dir, &err)) {
    state_ = ClientState::Disconnected;
    status_ = "could not build the server's scene: " + err;
    transport_->disconnect(server_, kReasonBadMessage);
    return;
  }
  world_.take_logs();  // the client world never runs scripts
  world_ready_ = true;
  slot_ = w.slot;
  rider_ = w.rider;
  project_name_ = w.project_name;
  state_ = ClientState::Joined;
  event("Rode into \"" + w.project_name + "\" as P" + std::to_string(w.slot + 1));
}

const runtime::NetActor* GameClient::row(const Snap& s, std::uint64_t id) const {
  for (const runtime::NetActor& a : s.actors) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

void GameClient::on_snapshot(SnapshotMsg&& m) {
  if (m.echo_time_ms != 0) {
    const double sample = static_cast<double>(now_ms() - m.echo_time_ms);
    if (sample >= 0.0 && sample < 10000.0) {
      stats_.rtt_ms = stats_.snapshots == 0 ? sample : stats_.rtt_ms * 0.9 + sample * 0.1;
    }
  }
  const double sample_offset =
      static_cast<double>(m.tick) - clock_ * static_cast<double>(runtime::kTickRate);
  if (!have_offset_ || sample_offset > tick_offset_) {
    tick_offset_ = sample_offset;
    have_offset_ = true;
  } else {
    tick_offset_ += (sample_offset - tick_offset_) * 0.02;
  }
  const bool newest = snaps_.empty() || m.tick > snaps_.back().tick;
  Snap s;
  s.tick = m.tick;
  s.actors = std::move(m.actors);
  if (!newest) {
    ++stats_.stale_snapshots;
    auto it = std::lower_bound(snaps_.begin(), snaps_.end(), s.tick,
                               [](const Snap& a, std::uint64_t t) { return a.tick < t; });
    if (it == snaps_.end() || it->tick != s.tick) snaps_.insert(it, std::move(s));
  } else {
    snaps_.push_back(std::move(s));
    ++stats_.snapshots;
    stats_.server_tick = m.tick;
    world_.net_apply(snaps_.back().actors);
    reconcile(snaps_.back(), m.ack_seq);
  }
  while (snaps_.size() > kMaxSnaps) snaps_.pop_front();
}

void GameClient::reconcile(const Snap& s, std::uint32_t ack) {
  acked_ = std::max(acked_, ack);
  while (!unacked_.empty() && unacked_.front().seq <= acked_) unacked_.pop_front();
  stats_.pending_inputs = unacked_.size();
  const runtime::NetActor* r = row(s, rider_);
  runtime::Actor* a = world_.find(rider_);
  if (!r || !a) return;
  const float old_x = pred_x_ + err_x_;
  const float old_y = pred_y_ + err_y_;
  a->data.x = r->x;
  a->data.y = r->y;
  for (const InputCmd& c : unacked_) {
    world_.predict_rider(rider_, c.input);
  }
  a = world_.find(rider_);
  if (!a) return;
  pred_x_ = a->data.x;
  pred_y_ = a->data.y;
  if (!have_pred_) {
    have_pred_ = true;
    err_x_ = err_y_ = 0.0f;
    return;
  }
  err_x_ = old_x - pred_x_;
  err_y_ = old_y - pred_y_;
  const float d = std::sqrt(err_x_ * err_x_ + err_y_ * err_y_);
  if (d > 0.01f) {
    ++stats_.corrections;
    stats_.last_correction_px = d;
    stats_.max_correction_px = std::max(stats_.max_correction_px, d);
  }
  if (d > kSnapPx) err_x_ = err_y_ = 0.0f;
}

// --- Tick ---------------------------------------------------------------------------

void GameClient::send_input() {
  InputMsg m;
  m.client_time_ms = now_ms();
  m.rtt_ms = static_cast<std::uint16_t>(std::clamp(stats_.rtt_ms, 0.0, 65535.0));
  for (auto it = unacked_.rbegin(); it != unacked_.rend() && m.cmds.size() < static_cast<std::size_t>(kInputRedundancy);
       ++it) {
    m.cmds.push_back(*it);
  }
  transport_->send(server_, Delivery::Unreliable, encode(m));
}

void GameClient::send_plugin_posts() {
  if (!transport_ || !joined()) return;
  for (runtime::NetPost& post : world_.take_net_posts()) {
    PluginMsg m;
    m.channel = std::move(post.channel);
    m.bytes = std::move(post.bytes);
    transport_->send(server_, net::Delivery::Reliable, encode(m));
  }
}

void GameClient::tick(const runtime::PlayerInput& input) {
  poll();
  send_plugin_posts();
  if (!transport_ || !joined() || !world_ready_ || !have_pred_) {
    if (transport_) transport_->flush();
    return;
  }
  last_input_ = input;
  last_input_.connected = true;
  InputCmd c;
  c.seq = ++seq_;
  c.input = last_input_;
  // Quantize like the wire so prediction replays exactly what the server runs.
  c.input.move_x = std::round(std::clamp(c.input.move_x, -1.0f, 1.0f) * 127.0f) / 127.0f;
  c.input.move_y = std::round(std::clamp(c.input.move_y, -1.0f, 1.0f) * 127.0f) / 127.0f;
  c.input.buttons &= 0xFFu;
  unacked_.push_back(c);
  while (unacked_.size() > kMaxUnacked) unacked_.pop_front();
  stats_.pending_inputs = unacked_.size();
  send_input();
  transport_->flush();

  if (runtime::Actor* a = world_.find(rider_)) {
    a->data.x = pred_x_;
    a->data.y = pred_y_;
    world_.predict_rider(rider_, c.input);
    if ((a = world_.find(rider_)) != nullptr) {
      pred_x_ = a->data.x;
      pred_y_ = a->data.y;
    }
  }
  err_x_ *= kSmoothDecay;
  err_y_ *= kSmoothDecay;
  if (std::fabs(err_x_) < 0.01f) err_x_ = 0.0f;
  if (std::fabs(err_y_) < 0.01f) err_y_ = 0.0f;
  pose();
  world_.net_animate();
}

int GameClient::update(double real_seconds, const runtime::PlayerInput& input) {
  real_seconds = std::clamp(real_seconds, 0.0, 0.25);
  clock_ += real_seconds;
  accumulator_ += real_seconds;
  int n = 0;
  while (accumulator_ >= runtime::kTickSeconds && n < 8) {
    accumulator_ -= runtime::kTickSeconds;
    tick(input);
    ++n;
  }
  if (n == 8) accumulator_ = 0.0;
  if (n == 0) poll();
  pose();
  measure(real_seconds);
  return n;
}

void GameClient::pose() {
  if (!world_ready_ || snaps_.empty()) return;
  const double interp_ticks = config_.interp_ms * runtime::kTickRate / 1000.0;
  const double rt = clock_ * runtime::kTickRate + tick_offset_ - interp_ticks;
  // Bracket the render tick: a <= rt < b.
  const Snap* a = &snaps_.front();
  const Snap* b = nullptr;
  for (const Snap& s : snaps_) {
    if (static_cast<double>(s.tick) <= rt) {
      a = &s;
    } else {
      b = &s;
      break;
    }
  }
  if (!b) b = a;  // ran out: hold the newest
  double t = 0.0;
  if (b != a && b->tick > a->tick) {
    t = std::clamp((rt - static_cast<double>(a->tick)) / static_cast<double>(b->tick - a->tick),
                   0.0, 1.0);
  }
  std::vector<runtime::NetActor> rows = b->actors;
  if (t < 1.0) {
    for (runtime::NetActor& r : rows) {
      if (const runtime::NetActor* o = row(*a, r.id)) {
        r.x = static_cast<float>(o->x + (r.x - o->x) * t);
        r.y = static_cast<float>(o->y + (r.y - o->y) * t);
        if (t < 0.5) {
          r.hidden = o->hidden;
          r.in_x = o->in_x;
          r.in_y = o->in_y;
          r.dir = o->dir;
          r.facing = o->facing;
        }
      }
    }
  }
  // Collider switches (an opened gate) follow the newest word right away:
  // prediction walks against them.
  const Snap& newest = snaps_.back();
  for (runtime::NetActor& r : rows) {
    if (const runtime::NetActor* n = row(newest, r.id)) r.collider_on = n->collider_on;
    if (r.id == rider_ && have_pred_) {
      r.x = pred_x_ + err_x_;
      r.y = pred_y_ + err_y_;
      r.in_x = last_input_.move_x;
      r.in_y = last_input_.move_y;
    }
  }
  world_.net_apply(rows);
}

void GameClient::measure(double real_seconds) {
  measure_window_ += real_seconds;
  if (profiler::Profiler::get().enabled()) {
    profiler::Profiler& p = profiler::Profiler::get();
    p.counter("net.rtt_ms", stats_.rtt_ms);
    p.counter("net.kbps_in", stats_.kbps_in);
    p.counter("net.kbps_out", stats_.kbps_out);
    p.counter("net.snapshot_hz", stats_.snapshot_hz);
    p.counter("net.corrections", static_cast<double>(stats_.corrections));
    p.counter("net.pending_inputs", static_cast<double>(stats_.pending_inputs));
    p.counter("net.interp_ms", stats_.interp_ms);
    p.counter("net.stale_snapshots", static_cast<double>(stats_.stale_snapshots));
  }
  if (measure_window_ < 1.0 || !transport_) return;
  const net::LinkStats s = transport_->stats(server_);
  stats_.kbps_in = static_cast<double>(s.bytes_received - std::min(s.bytes_received, bytes_in_mark_)) /
                   1024.0 / measure_window_;
  stats_.kbps_out = static_cast<double>(s.bytes_sent - std::min(s.bytes_sent, bytes_out_mark_)) /
                    1024.0 / measure_window_;
  bytes_in_mark_ = s.bytes_received;
  bytes_out_mark_ = s.bytes_sent;
  stats_.snapshot_hz = static_cast<double>(stats_.snapshots - measure_snaps_) / measure_window_;
  measure_snaps_ = stats_.snapshots;
  measure_window_ = 0.0;
}

bool GameClient::server_rider(float* x, float* y) const { return latest(rider_, x, y); }

bool GameClient::latest(std::uint64_t id, float* x, float* y) const {
  if (snaps_.empty()) return false;
  const runtime::NetActor* r = row(snaps_.back(), id);
  if (!r) return false;
  *x = r->x;
  *y = r->y;
  return true;
}

bool GameClient::drawn(std::uint64_t id, float* x, float* y) const {
  const runtime::Actor* a = world_.find(id);
  if (!a || a->dead) return false;
  *x = a->data.x;
  *y = a->data.y;
  return true;
}

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
