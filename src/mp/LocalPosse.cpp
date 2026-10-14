#include "mp/LocalPosse.h"

#include <algorithm>

namespace ts {
namespace tombstone {
namespace mp {

const char* posse_name(int index) {
  static const char* kNames[] = {"Ike", "Doc", "Belle", "Kid", "Wyatt", "Calamity",
                                 "Hoss", "Annie"};
  return kNames[static_cast<std::size_t>(index) % (sizeof(kNames) / sizeof(kNames[0]))];
}

runtime::PlayerInput bot_input(int rider_index, std::uint64_t tick) {
  runtime::PlayerInput in;
  in.connected = true;
  const std::uint64_t phase = (tick / 45 + static_cast<std::uint64_t>(rider_index) * 2) % 5;
  switch (phase) {
    case 0: in.move_x = 1.0f; break;
    case 1: in.move_y = 1.0f; break;
    case 2: in.move_x = -1.0f; break;
    case 3: in.move_y = -1.0f; break;
    default: break;  // let the horse breathe
  }
  return in;
}

bool LocalPosse::start(PosseConfig config, std::string* error_out) {
  stop();
  config_ = std::move(config);
  config_.riders = std::clamp(config_.riders, 1, kMaxPosse);
  ServerConfig sc;
  sc.project_dir = config_.project_dir;
  sc.scene = config_.scene;
  sc.project_name = config_.project_name;
  sc.port = config_.port;
  sc.snapshot_every = config_.snapshot_every;
  if (!server_.start(std::move(sc), error_out)) return false;
  names_used_ = 0;
  bot_tick_ = 0;
  bot_acc_ = 0.0;
  for (int i = 0; i < config_.riders; ++i) {
    if (!add_rider(error_out)) {
      stop();
      return false;
    }
  }
  return true;
}

void LocalPosse::stop() {
  for (auto& c : clients_) c->disconnect();
  clients_.clear();
  server_.stop();
}

bool LocalPosse::add_rider(std::string* error_out) {
  if (!server_.running()) return false;
  if (static_cast<int>(clients_.size()) >= kMaxPosse) {
    if (error_out) *error_out = "The posse is full: four riders already.";
    return false;
  }
  ClientConfig cc;
  cc.host = "127.0.0.1";
  cc.port = server_.port();
  cc.name = posse_name(names_used_++);
  cc.project_dir = config_.project_dir;
  cc.sim = config_.sim;
  cc.interp_ms = config_.interp_ms;
  auto c = std::make_unique<GameClient>();
  if (!c->connect(std::move(cc), error_out)) return false;
  clients_.push_back(std::move(c));
  return true;
}

bool LocalPosse::remove_rider() {
  if (clients_.empty()) return false;
  clients_.back()->disconnect();
  clients_.pop_back();
  return true;
}

void LocalPosse::set_link_sim(const net::LinkSim& sim) {
  config_.sim = sim;
  for (auto& c : clients_) c->set_link_sim(sim);
}

void LocalPosse::update(double real_seconds, const runtime::PlayerInput& p1) {
  if (!server_.running()) return;
  bot_acc_ += real_seconds;
  while (bot_acc_ >= runtime::kTickSeconds) {
    bot_acc_ -= runtime::kTickSeconds;
    ++bot_tick_;
  }
  server_.update(real_seconds);
  for (std::size_t i = 0; i < clients_.size(); ++i) {
    runtime::PlayerInput in = p1;
    if (i > 0) {
      in = bots_ ? bot_input(static_cast<int>(i), bot_tick_) : runtime::PlayerInput{};
    }
    clients_[i]->update(real_seconds, in);
  }
  server_.poll();
}

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
