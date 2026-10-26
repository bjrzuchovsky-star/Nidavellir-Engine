#include "team/TeamRpc.h"

#include <chrono>

namespace ts {
namespace tombstone {
namespace team {

bool TeamRpc::call(json::Value req, json::Value* reply, double timeout_s, std::string* error) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!open_) {
    if (error) *error = why_;
    return false;
  }
  const std::uint64_t rid = next_rid_++;
  const std::uint64_t epoch = epoch_;
  req.set("rid", json::Value::number(static_cast<double>(rid)));
  out_.push_back(std::move(req));
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
  while (true) {
    auto it = replies_.find(rid);
    if (it != replies_.end()) {
      json::Value r = std::move(it->second);
      replies_.erase(it);
      if (!r.flag("ok", false)) {
        if (error) *error = r.str("error", "The team host said no.");
        if (reply) *reply = std::move(r);
        return false;
      }
      if (reply) *reply = std::move(r);
      return true;
    }
    if (epoch != epoch_ || !open_) {
      if (error) *error = why_;
      return false;
    }
    if (cv_.wait_until(lock, until) == std::cv_status::timeout && !replies_.count(rid)) {
      if (error) *error = "The team host did not answer in time.";
      return false;
    }
  }
}

void TeamRpc::open() {
  std::lock_guard<std::mutex> lock(mu_);
  open_ = true;
}

void TeamRpc::close(const std::string& why) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!open_) return;
    open_ = false;
    why_ = why;
    ++epoch_;
    out_.clear();
    replies_.clear();
  }
  cv_.notify_all();
}

bool TeamRpc::is_open() const {
  std::lock_guard<std::mutex> lock(mu_);
  return open_;
}

std::vector<json::Value> TeamRpc::take_outgoing() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<json::Value> out(std::make_move_iterator(out_.begin()), std::make_move_iterator(out_.end()));
  out_.clear();
  return out;
}

void TeamRpc::deliver(const json::Value& reply) {
  const double rid = reply.num("rid", 0);
  if (rid <= 0) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!open_) return;
    replies_[static_cast<std::uint64_t>(rid)] = reply;
  }
  cv_.notify_all();
}

}  // namespace team
}  // namespace tombstone
}  // namespace ts
