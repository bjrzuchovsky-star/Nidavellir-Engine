#include "editor/workspace/Workspace2D.h"

#include <algorithm>
#include <utility>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

const std::string kEmptyLabel;

}  // namespace

Workspace2D::Snapshot Workspace2D::snapshot() const {
  Snapshot s;
  s.entities = entities_;
  s.tile_solidity = tile_solidity_;
  s.selected_id = selected_id_;
  s.selection = selection_;
  return s;
}

void Workspace2D::restore(const Snapshot& s) {
  entities_ = s.entities;
  tile_solidity_ = s.tile_solidity;
  move_starts_.clear();
  move_active_ = false;
  move_changed_ = false;
  // Keep ids monotonic so a redo-able or re-created entity never collides
  // with one that was allocated after it.
  std::uint64_t max_id = 0;
  for (const Entity2D& e : entities_) {
    max_id = std::max(max_id, e.id);
  }
  if (max_id + 1 > next_id_) {
    next_id_ = max_id + 1;
  }
  selection_ = s.selection;
  selected_id_ = s.selected_id;
  prune_selection();
}

bool Workspace2D::same_entities(const Snapshot& s) const {
  return s.entities == entities_ && s.tile_solidity == tile_solidity_;
}

bool Workspace2D::commit_step(std::string label, Snapshot before) {
  if (same_entities(before)) {
    return false;
  }
  undo_.push_back({std::move(label), std::move(before)});
  while (undo_.size() > kMaxHistory) {
    undo_.pop_front();
  }
  redo_.clear();
  return true;
}

void Workspace2D::begin_edit(std::string label) {
  if (pending_) {
    return;
  }
  begin_edit(std::move(label), snapshot());
}

void Workspace2D::begin_edit(std::string label, Snapshot before) {
  if (pending_) {
    return;
  }
  pending_ = std::move(before);
  pending_label_ = std::move(label);
}

void Workspace2D::set_edit_label(std::string label) {
  if (pending_) {
    pending_label_ = std::move(label);
  }
}

bool Workspace2D::commit_edit() {
  if (!pending_) {
    return false;
  }
  Snapshot before = std::move(*pending_);
  pending_.reset();
  std::string label = std::move(pending_label_);
  pending_label_.clear();
  return commit_step(std::move(label), std::move(before));
}

bool Workspace2D::revert_edit() {
  if (!pending_) {
    return false;
  }
  Snapshot before = std::move(*pending_);
  pending_.reset();
  pending_label_.clear();
  restore(before);
  return true;
}

void Workspace2D::cancel_edit() {
  pending_.reset();
  pending_label_.clear();
}

void Workspace2D::for_each_history_state(
    const std::function<void(std::vector<Entity2D>&, TileSolidity&)>& fn) {
  for (HistoryStep& h : undo_) fn(h.state.entities, h.state.tile_solidity);
  for (HistoryStep& h : redo_) fn(h.state.entities, h.state.tile_solidity);
  if (pending_) fn(pending_->entities, pending_->tile_solidity);
}

const std::string& Workspace2D::undo_label() const {
  return undo_.empty() ? kEmptyLabel : undo_.back().label;
}

const std::string& Workspace2D::redo_label() const {
  return redo_.empty() ? kEmptyLabel : redo_.back().label;
}

bool Workspace2D::undo(std::string* label_out) {
  if (move_active_) {
    cancel_move();  // a half-finished drag is dropped, not committed
  }
  commit_edit();
  if (undo_.empty()) {
    return false;
  }
  HistoryStep step = std::move(undo_.back());
  undo_.pop_back();
  redo_.push_back({step.label, snapshot()});
  restore(step.state);
  if (label_out) {
    *label_out = step.label;
  }
  return true;
}

bool Workspace2D::redo(std::string* label_out) {
  if (move_active_) {
    cancel_move();  // a half-finished drag is dropped, not committed
  }
  commit_edit();
  if (redo_.empty()) {
    return false;
  }
  HistoryStep step = std::move(redo_.back());
  redo_.pop_back();
  undo_.push_back({step.label, snapshot()});
  while (undo_.size() > kMaxHistory) {
    undo_.pop_front();
  }
  restore(step.state);
  if (label_out) {
    *label_out = step.label;
  }
  return true;
}

void Workspace2D::clear_history() {
  undo_.clear();
  redo_.clear();
  cancel_edit();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
