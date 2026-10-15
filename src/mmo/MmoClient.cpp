#include "mmo/MmoClient.h"

#include "mmo/MmoServer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ts {
namespace tombstone {
namespace mmo {

const char* to_string(MmoClient::State s) {
  switch (s) {
    case MmoClient::State::Idle: return "idle";
    case MmoClient::State::Connecting: return "connecting";
    case MmoClient::State::LoggingIn: return "logging in";
    case MmoClient::State::InWorld: return "in world";
    case MmoClient::State::Reconnecting: return "reconnecting";
    case MmoClient::State::Failed: return "failed";
    case MmoClient::State::Closed: return "closed";
  }
  return "?";
}

MmoClient::MmoClient() : make_([] { return net::make_enet_transport(); }) {}

MmoClient::~MmoClient() {
  if (transport_) transport_->close();
}

double MmoClient::now_ms() const {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void MmoClient::send(net::Delivery d, const std::vector<std::uint8_t>& msg) {
  if (transport_ && server_ != net::kNoPeer && transport_->send(server_, d, msg)) stats_.bytes_out += msg.size();
}

bool MmoClient::open(const std::string& host, std::uint16_t port, LoginMsg msg) {
  if (transport_) transport_->close();
  transport_ = make_();
  host_ = host;
  port_ = port;
  pending_login_ = std::move(msg);
  error_.clear();
  reason_ = 0;
  quitting_ = false;
  std::string err;
  server_ = transport_->connect(host, port, &err);
  if (server_ == net::kNoPeer) {
    error_ = err.empty() ? "could not connect" : err;
    state_ = State::Failed;
    return false;
  }
  state_ = State::Connecting;
  return true;
}

bool MmoClient::login(const std::string& host, std::uint16_t port, const std::string& user,
                      const std::string& password) {
  LoginMsg m;
  m.mode = LoginMode::Login;
  m.user = user;
  m.password = password;
  return open(host, port, std::move(m));
}

bool MmoClient::create(const std::string& host, std::uint16_t port, const std::string& user,
                       const std::string& password, const std::string& character) {
  LoginMsg m;
  m.mode = LoginMode::Create;
  m.user = user;
  m.password = password;
  m.character = character;
  return open(host, port, std::move(m));
}

bool MmoClient::resume(const std::string& host, std::uint16_t port, const std::string& token) {
  LoginMsg m;
  m.mode = LoginMode::Resume;
  m.token = token;
  return open(host, port, std::move(m));
}

void MmoClient::quit() {
  if (!transport_ || server_ == net::kNoPeer) return;
  quitting_ = true;
  transport_->disconnect(server_, kMmoQuit);
  for (int i = 0; i < 10; ++i) {
    transport_->flush();
    net::TransportEvent ev;
    while (transport_->poll(&ev)) {
    }
  }
  state_ = State::Closed;
}

void MmoClient::drop_line() {
  if (!transport_) return;
  transport_->close();  // no goodbye: the server times us out
  transport_.reset();
  server_ = net::kNoPeer;
  if (state_ == State::InWorld && auto_resume_ && !session_.token.empty()) {
    state_ = State::Reconnecting;
    reconnect_at_ = clock_ + 0.2;
  } else {
    state_ = State::Closed;
  }
}

void MmoClient::say(const std::string& channel, const std::string& text) {
  ChatSendMsg m;
  m.channel = channel;
  m.text = text;
  send(net::Delivery::Reliable, encode(m));
}

void MmoClient::interact(std::uint64_t entity) {
  InteractMsg m;
  m.entity = entity;
  send(net::Delivery::Reliable, encode(m));
}

void MmoClient::handle(const net::TransportEvent& ev) {
  using T = net::TransportEvent::Type;
  if (ev.type == T::Connected) {
    state_ = State::LoggingIn;
    send(net::Delivery::Reliable, encode(pending_login_));
    pending_login_.password.assign(pending_login_.password.size(), '\0');
    pending_login_.password.clear();
    return;
  }
  if (ev.type == T::Disconnected) {
    reason_ = ev.reason;
    server_ = net::kNoPeer;
    const bool final_reason = ev.reason == kMmoKicked || ev.reason == kMmoCheat || ev.reason == kMmoBanned ||
                              ev.reason == kMmoElsewhere || ev.reason == kMmoLoginFailed || quitting_;
    if (state_ == State::InWorld && auto_resume_ && !final_reason && !session_.token.empty()) {
      state_ = State::Reconnecting;
      reconnect_at_ = clock_ + 0.5;
    } else if ((state_ == State::Reconnecting || state_ == State::Connecting || state_ == State::LoggingIn) &&
               reconnect_tries_ > 0 && reconnect_tries_ < 6 && !final_reason) {
      state_ = State::Reconnecting;
      reconnect_at_ = clock_ + 0.5 * reconnect_tries_;
    } else {
      if (error_.empty()) error_ = "disconnected (" + std::to_string(ev.reason) + ")";
      state_ = state_ == State::InWorld || quitting_ ? State::Closed : State::Failed;
    }
    return;
  }
  stats_.bytes_in += ev.data.size();
  MmoMsg type;
  if (!peek(ev.data, &type)) return;
  switch (type) {
    case MmoMsg::LoginOk: {
      LoginOkMsg m;
      if (!decode(ev.data, &m)) return;
      if (m.resumed && state_ == State::LoggingIn && reconnect_tries_ > 0) ++stats_.resumes;
      session_ = m;
      reconnect_tries_ = 0;
      last_snap_tick_ = 0;
      seq_ = 0;
      unacked_.clear();
      ents_.clear();
      state_ = State::InWorld;
      break;
    }
    case MmoMsg::LoginFail: {
      LoginFailMsg m;
      if (decode(ev.data, &m)) error_ = m.text;
      state_ = State::Failed;
      break;
    }
    case MmoMsg::Zone: {
      ZoneMsg m;
      if (!decode(ev.data, &m)) return;
      zone_ = m.geo;
      x_ = m.x;
      y_ = m.y;
      unacked_.clear();
      ents_.clear();
      break;
    }
    case MmoMsg::Snapshot: {
      SnapshotMsg m;
      if (!decode(ev.data, &m) || m.tick < last_snap_tick_) return;  // late: newer won
      last_snap_tick_ = m.tick;
      ++stats_.snapshots;
      if (m.echo_ms) {
        const double rtt = std::fmod(now_ms(), 4294967296.0) - m.echo_ms;
        if (rtt >= 0 && rtt < 10000) stats_.rtt_ms = static_cast<std::uint32_t>(rtt);
      }
      for (const SnapRow& r : m.rows) {
        auto it = ents_.find(r.id);
        if (it != ents_.end()) {
          it->second.x = r.x;
          it->second.y = r.y;
        }
      }
      // Reconcile: the server's position, then replay what it hasn't seen.
      while (!unacked_.empty() && unacked_.front().seq <= m.ack) unacked_.pop_front();
      if (m.corrected) ++stats_.corrections;
      float x = m.x, y = m.y;
      const float dt = 1.0f / static_cast<float>(std::max(1, session_.tick_rate));
      for (const MoveCmd& c : unacked_) step_rider(zone_, session_.speed, dt, c.mx, c.my, &x, &y);
      x_ = x;
      y_ = y;
      break;
    }
    case MmoMsg::Appear: {
      AppearMsg m;
      if (!decode(ev.data, &m)) return;
      for (const AppearRow& a : m.rows) ents_[a.id] = ClientEntity{a.id, a.kind, a.name, a.x, a.y};
      stats_.appears += m.rows.size();
      break;
    }
    case MmoMsg::Vanish: {
      VanishMsg m;
      if (!decode(ev.data, &m)) return;
      for (std::uint64_t id : m.ids) ents_.erase(id);
      stats_.vanishes += m.ids.size();
      break;
    }
    case MmoMsg::Chat: {
      ChatMsg m;
      if (!decode(ev.data, &m)) return;
      inbox_.push_back(std::move(m));
      while (inbox_.size() > 200) inbox_.pop_front();
      break;
    }
    case MmoMsg::Inventory: {
      InventoryMsg m;
      if (decode(ev.data, &m)) inventory_ = m.items;
      break;
    }
    default:
      break;
  }
}

void MmoClient::client_tick(double dt) {
  if (state_ != State::InWorld) return;
  const int moves = 1 + std::max(0, flood_);
  for (int i = 0; i < moves; ++i) {
    MoveCmd c;
    c.seq = ++seq_;
    c.mx = quantize_intent(mx_);
    c.my = quantize_intent(my_);
    step_rider(zone_, session_.speed, static_cast<float>(dt), c.mx, c.my, &x_, &y_);
    unacked_.push_back(c);
  }
  while (unacked_.size() > 120) unacked_.pop_front();
  MoveMsg m;
  for (auto it = unacked_.rbegin(); it != unacked_.rend() && m.cmds.size() < static_cast<std::size_t>(kMoveRedundancy + flood_); ++it)
    m.cmds.push_back(*it);
  m.claim_x = x_ + cheat_dx_;
  m.claim_y = y_ + cheat_dy_;
  m.client_ms = static_cast<std::uint32_t>(std::fmod(now_ms(), 4294967296.0));
  m.rtt_ms = static_cast<std::uint16_t>(std::min<std::uint32_t>(stats_.rtt_ms, 65535));
  send(net::Delivery::Unreliable, encode(m));
}

void MmoClient::update(double real_seconds) {
  clock_ += real_seconds;
  if (state_ == State::Reconnecting && clock_ >= reconnect_at_ && !transport_) {
    ++reconnect_tries_;
    LoginMsg m;
    m.mode = LoginMode::Resume;
    m.token = session_.token;
    const State keep = State::Reconnecting;
    if (!open(host_, port_, std::move(m))) {
      state_ = reconnect_tries_ < 6 ? keep : State::Failed;
      transport_.reset();
      reconnect_at_ = clock_ + 0.5 * reconnect_tries_;
    } else {
      state_ = State::Connecting;
    }
  } else if (state_ == State::Reconnecting && transport_ && clock_ >= reconnect_at_) {
    transport_->close();
    transport_.reset();
  }
  if (!transport_) return;
  net::TransportEvent ev;
  while (transport_ && transport_->poll(&ev)) handle(ev);
  if (state_ == State::InWorld) {
    const double dt = 1.0 / std::max(1, session_.tick_rate);
    accumulator_ += std::clamp(real_seconds, 0.0, 0.25);
    while (accumulator_ >= dt) {
      accumulator_ -= dt;
      client_tick(dt);
    }
  }
  if (transport_) transport_->flush();
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
