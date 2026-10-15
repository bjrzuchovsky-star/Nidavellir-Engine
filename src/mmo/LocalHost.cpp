#include "mmo/LocalHost.h"

#include <chrono>

namespace ts {
namespace tombstone {
namespace mmo {

LocalHost::~LocalHost() { stop(); }

bool LocalHost::start(MmoServerConfig cfg, std::string* error) {
  stop();
  server_ = std::make_unique<MmoServer>();
  if (!server_->start(std::move(cfg), error)) {
    server_.reset();
    return false;
  }
  quit_ = false;
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void LocalHost::stop() {
  if (thread_.joinable()) {
    quit_ = true;
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (swarm_) {
    swarm_->quit_all();
    swarm_.reset();
  }
  if (server_) {
    server_->stop();
    server_.reset();
  }
  running_ = false;
  want_bots_ = 0;
}

void LocalHost::set_bots(int n) {
  std::lock_guard<std::mutex> lock(mu_);
  want_bots_ = std::max(0, n);
  if (!server_) return;
  if (want_bots_ == 0) {
    if (swarm_) swarm_->quit_all();
    swarm_.reset();
    return;
  }
  if (swarm_ && swarm_->size() > want_bots_) {
    swarm_->quit_all();
    swarm_.reset();
  }
  if (!swarm_) {
    BotOptions bo;
    bo.port = server_->port();
    bo.prefix = "bot";
    bo.seed = static_cast<std::uint32_t>(++swarm_gen_ * 7919);
    swarm_ = std::make_unique<BotSwarm>(bo);
  }
  if (swarm_->size() < want_bots_) swarm_->add(want_bots_ - swarm_->size());
}

void LocalHost::with(const std::function<void(MmoServer&, BotSwarm*)>& fn) {
  std::lock_guard<std::mutex> lock(mu_);
  if (server_) fn(*server_, swarm_.get());
}

std::string LocalHost::admin(const std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);
  return server_ ? server_->admin(line, "console") : std::string("the Town Hall is closed");
}

void LocalHost::run() {
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now();
  while (!quit_) {
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (server_) {
        server_->update(dt);
        if (swarm_) swarm_->update(dt);
        if (server_->shutdown_requested()) quit_ = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  running_ = false;
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
