#pragma once

// Abstract per-player input. The runtime never reads a keyboard or a
// gamepad itself: the editor (ImGui keys), ts_game (GLFW) and later the
// network layer all fill an InputFrame and hand it to World::step().

#include "scene/SceneData.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ts {
namespace tombstone {
namespace runtime {

// Action buttons (bit flags). Names are roles, not keys.
enum Button : std::uint32_t {
  kButtonAction = 1u << 0,  // E / Space / gamepad A
  kButtonAlt = 1u << 1,     // Shift / gamepad B
  kButtonStart = 1u << 2,   // Enter / gamepad Start
  // Gameplay plugins (Frontier Survival): the network carries 8 bits.
  kButtonUse = 1u << 3,      // F / gamepad X: mine, swing, eat
  kButtonPlace = 1u << 4,    // G / gamepad Y: place the held block
  kButtonFire = 1u << 5,     // R / right trigger: ranged weapon
  kButtonNext = 1u << 6,     // X / right shoulder: next hotbar slot
  kButtonPrev = 1u << 7,     // Z / left shoulder: previous hotbar slot
};

struct PlayerInput {
  float move_x = 0.0f;  // -1 left .. +1 right
  float move_y = 0.0f;  // -1 up .. +1 down (world y grows down)
  std::uint32_t buttons = 0;
  bool connected = false;  // a device / client is driving this slot
  // Top-down "face mouse" riders: the world point to face (the mouse
  // cursor under the camera, or a point along the right stick). Local
  // only: the network does not carry it.
  bool aim = false;
  float aim_x = 0.0f;
  float aim_y = 0.0f;

  bool held(Button b) const { return (buttons & b) != 0; }
};

// One fixed tick's worth of input for every slot.
struct InputFrame {
  std::array<PlayerInput, kMaxPlayers> players{};

  PlayerInput& slot(int i) { return players[static_cast<std::size_t>(i)]; }
  const PlayerInput& slot(int i) const {
    return players[static_cast<std::size_t>(i)];
  }
};

// Keys / d-pad to a move vector (opposites cancel).
inline PlayerInput digital_input(bool left, bool right, bool up, bool down) {
  PlayerInput in;
  in.move_x = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
  in.move_y = (down ? 1.0f : 0.0f) - (up ? 1.0f : 0.0f);
  in.connected = true;
  return in;
}

// Combine two sources for one slot (keyboard + gamepad): the larger move
// on each axis wins, buttons OR together.
inline PlayerInput merge_input(const PlayerInput& a, const PlayerInput& b) {
  auto pick = [](float u, float v) {
    return (u < 0 ? -u : u) >= (v < 0 ? -v : v) ? u : v;
  };
  PlayerInput out;
  out.move_x = pick(a.move_x, b.move_x);
  out.move_y = pick(a.move_y, b.move_y);
  out.buttons = a.buttons | b.buttons;
  out.connected = a.connected || b.connected;
  // The latest aim source wins (a pushed stick over the mouse).
  const PlayerInput& aim = b.aim ? b : a;
  out.aim = aim.aim;
  out.aim_x = aim.aim_x;
  out.aim_y = aim.aim_y;
  return out;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
