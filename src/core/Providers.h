#pragma once

// Process-wide registry of optional engine parts that Trading Post plugins
// switch on: physics backends (Jolt, Box2D) and rider controllers.
//
//   * A host *offers* an implementation by id (a factory). Offering does not
//     switch anything on; it only says "this binary can do it".
//   * A plugin *acquires* its id in on_load and releases it in on_unload
//     (ref-counted: two plugin managers, e.g. the open claim and the template
//     thumbnailer, can hold the same id).
//   * get(id) / current() hand out an implementation only when its id is both
//     offered and acquired. Nothing acquired = the core's no-op behaviour.
//
// Header-only; every engine binary links its plugins statically, so there
// is exactly one registry per process.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace core {

template <typename T>
class Providers {
 public:
  using Factory = std::function<T()>;

  static Providers& instance() {
    static Providers p;
    return p;
  }

  // Idempotent; a second offer for the same id replaces the factory.
  void offer(const std::string& id, Factory make) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[id];
    if (!e.offered) order_.push_back(id);
    e.offered = true;
    e.make = std::move(make);
    e.cached = T{};
    ++generation_;
  }
  void acquire(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[id];
    if (!e.offered && e.refs == 0 &&
        std::find(order_.begin(), order_.end(), id) == order_.end()) {
      order_.push_back(id);
    }
    ++e.refs;
    ++generation_;
  }
  void release(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.refs <= 0) return;
    if (--it->second.refs == 0) it->second.cached = T{};
    ++generation_;
  }
  bool offered(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.offered;
  }
  bool active(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.offered && it->second.refs > 0;
  }
  int refs(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? 0 : it->second.refs;
  }
  // The implementation for `id` (made once per activation), or T{}.
  T get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    return get_locked(id);
  }
  // First active provider in offer order (physics: one backend at a time).
  T current(std::string* id_out = nullptr) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const std::string& id : order_) {
      auto it = entries_.find(id);
      if (it == entries_.end() || !it->second.offered || it->second.refs <= 0) continue;
      if (id_out) *id_out = id;
      return get_locked(id);
    }
    if (id_out) id_out->clear();
    return T{};
  }
  std::vector<std::string> active_ids() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (const std::string& id : order_) {
      auto it = entries_.find(id);
      if (it != entries_.end() && it->second.offered && it->second.refs > 0) out.push_back(id);
    }
    return out;
  }
  // Bumps on every offer / acquire / release (cheap change detection).
  std::uint64_t generation() const {
    std::lock_guard<std::mutex> lock(mu_);
    return generation_;
  }

 private:
  struct Entry {
    bool offered = false;
    int refs = 0;
    Factory make;
    T cached{};
  };
  T get_locked(const std::string& id) {
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second.offered || it->second.refs <= 0) return T{};
    Entry& e = it->second;
    if (!e.cached && e.make) e.cached = e.make();
    return e.cached;
  }

  mutable std::mutex mu_;
  std::map<std::string, Entry> entries_;
  std::vector<std::string> order_;
  std::uint64_t generation_ = 0;
};

}  // namespace core
}  // namespace tombstone
}  // namespace ts
