#pragma once

// Saddlebags: a fixed grid of item stacks. The first kHotbar slots are the
// hotbar. Item indices refer to GameData::items.

#include "plugins/survival/Data.h"

#include <array>
#include <cstdint>

namespace ts {
namespace tombstone {
namespace survival {

struct Stack {
  int item = -1;  // -1 = empty
  int count = 0;
  bool empty() const { return item < 0 || count <= 0; }
  bool operator==(const Stack& o) const = default;
};

class Inventory {
 public:
  static constexpr int kSlots = 30;
  static constexpr int kHotbar = 10;

  const Stack& at(int i) const { return slots_[static_cast<std::size_t>(i)]; }
  Stack& at(int i) { return slots_[static_cast<std::size_t>(i)]; }
  const std::array<Stack, kSlots>& slots() const { return slots_; }

  int count(int item) const;
  // Adds what fits (stacks first, then empty slots, hotbar first); returns
  // how many did NOT fit.
  int add(const GameData& data, int item, int n);
  // Removes up to n; returns how many were removed.
  int remove(int item, int n);
  bool has(int item, int n) const { return count(item) >= n; }
  void swap(int a, int b);
  void clear() { slots_ = {}; }
  bool operator==(const Inventory& o) const = default;

 private:
  std::array<Stack, kSlots> slots_{};
};

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
