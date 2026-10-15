#pragma once

// In-game debug tools (ts_debugtools) for any host with a ui::UiSystem
// (ts_game 2D / 3D, editor Play): a non-modal overlay (F3) with fps,
// frame / GPU times, the slowest scopes, draws, memory, assets and net,
// and a console (backtick) that runs profiler::Console commands. Both are
// built-in Dusk documents ("debug_overlay", "debug_console") added unless
// the project ships its own with the same widget ids. The profiler is
// switched on while either is open (or when the host says so).

#include "profiler/Console.h"
#include "ui/Ui.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace profiler {

std::string builtin_overlay_ui();
std::string builtin_console_ui();

class DebugTools {
 public:
  DebugTools() = default;
  ~DebugTools();
  DebugTools(const DebugTools&) = delete;
  DebugTools& operator=(const DebugTools&) = delete;
  // Adds the documents (when missing) and the overlay / clear commands to
  // the shared console (Console::shared()).
  void attach(ui::UiSystem& ui);
  void detach();
  bool attached() const { return ui_ != nullptr; }

  void set_overlay(bool on);
  bool overlay() const { return overlay_; }
  void set_console(bool on);
  bool console_open() const { return console_open_; }
  void toggle_overlay() { set_overlay(!overlay_); }
  void toggle_console() { set_console(!console_open_); }

  // The console's own events (submit / back / its buttons) are handled;
  // everything else comes back for the world.
  std::vector<ui::UiEvent> filter(std::vector<ui::UiEvent> events);
  // Refresh the overlay (4x / s) and the console log.
  void update(double dt);

  Console& console() { return console_; }
  // The overlay's text as it would show now (smokes, HUD fallbacks).
  static std::string overlay_text();

 private:
  void refresh_overlay();
  void refresh_console();

  ui::UiSystem* ui_ = nullptr;
  Console& console_ = Console::shared();
  bool registered_ = false;
  bool overlay_ = false;
  bool console_open_ = false;
  bool was_enabled_ = false;  // profiler state before we switched it on
  bool switched_on_ = false;
  double since_ = 1.0;
  std::size_t shown_log_ = static_cast<std::size_t>(-1);
};

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
