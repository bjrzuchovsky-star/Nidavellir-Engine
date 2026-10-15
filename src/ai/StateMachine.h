#pragma once

// A small finite-state-machine helper: named states with enter / update /
// exit callbacks; update returns the next state's name ("" = stay).
//
//   ai::StateMachine fsm;
//   fsm.add("idle", {}, [&](float) { return sees ? "chase" : ""; });
//   fsm.add("chase", [&] { roar(); }, [&](float dt) { ...; return lost ? "idle" : ""; });
//   fsm.start("idle");  fsm.update(dt);

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace ai {

class StateMachine {
 public:
  using Enter = std::function<void()>;
  using Update = std::function<std::string(float dt)>;
  using Exit = std::function<void()>;

  void add(const std::string& name, Enter enter, Update update, Exit exit = {}) {
    states_.push_back({name, std::move(enter), std::move(update), std::move(exit)});
  }
  bool start(const std::string& name) { return change(name); }
  // Switch now (exit the old state, enter the new one). False if unknown.
  bool change(const std::string& name) {
    const int to = index(name);
    if (to < 0) return false;
    if (cur_ >= 0 && states_[static_cast<std::size_t>(cur_)].exit) states_[static_cast<std::size_t>(cur_)].exit();
    prev_ = cur_;
    cur_ = to;
    time_ = 0.0f;
    ++changes_;
    if (states_[static_cast<std::size_t>(cur_)].enter) states_[static_cast<std::size_t>(cur_)].enter();
    return true;
  }
  void update(float dt) {
    if (cur_ < 0) return;
    time_ += dt;
    const State& s = states_[static_cast<std::size_t>(cur_)];
    if (!s.update) return;
    const std::string next = s.update(dt);
    if (!next.empty() && next != s.name) change(next);
  }
  std::string current() const { return cur_ >= 0 ? states_[static_cast<std::size_t>(cur_)].name : ""; }
  std::string previous() const { return prev_ >= 0 ? states_[static_cast<std::size_t>(prev_)].name : ""; }
  float time_in_state() const { return time_; }
  int changes() const { return changes_; }

 private:
  struct State {
    std::string name;
    Enter enter;
    Update update;
    Exit exit;
  };
  int index(const std::string& name) const {
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (states_[i].name == name) return static_cast<int>(i);
    }
    return -1;
  }
  std::vector<State> states_;
  int cur_ = -1;
  int prev_ = -1;
  float time_ = 0.0f;
  int changes_ = 0;
};

}  // namespace ai
}  // namespace ts
