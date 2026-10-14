#include "plugins/survival/Inventory.h"

#include <algorithm>
#include <utility>

namespace ts {
namespace tombstone {
namespace survival {

int Inventory::count(int item) const {
  int n = 0;
  for (const Stack& s : slots_) {
    if (s.item == item && s.count > 0) n += s.count;
  }
  return n;
}

int Inventory::add(const GameData& data, int item, int n) {
  const ItemDef* def = data.item_def(item);
  if (!def || n <= 0) return n;
  for (Stack& s : slots_) {
    if (n <= 0) break;
    if (s.item == item && s.count > 0 && s.count < def->stack) {
      const int put = std::min(n, def->stack - s.count);
      s.count += put;
      n -= put;
    }
  }
  for (Stack& s : slots_) {
    if (n <= 0) break;
    if (s.empty()) {
      const int put = std::min(n, def->stack);
      s.item = item;
      s.count = put;
      n -= put;
    }
  }
  return n;
}

int Inventory::remove(int item, int n) {
  int removed = 0;
  // Back to front: the hotbar keeps its stacks longest.
  for (int i = kSlots - 1; i >= 0 && removed < n; --i) {
    Stack& s = slots_[static_cast<std::size_t>(i)];
    if (s.item != item || s.count <= 0) continue;
    const int take = std::min(n - removed, s.count);
    s.count -= take;
    removed += take;
    if (s.count <= 0) s = Stack{};
  }
  return removed;
}

void Inventory::swap(int a, int b) {
  if (a < 0 || b < 0 || a >= kSlots || b >= kSlots) return;
  std::swap(slots_[static_cast<std::size_t>(a)], slots_[static_cast<std::size_t>(b)]);
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
