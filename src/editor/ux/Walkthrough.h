#pragma once

// First-run walkthrough: a short tour (8 steps per editor) that points at
// each panel in turn. Shown the first time each editor opens; Skip closes
// it for now, "Don't show again" (or finishing) marks it seen in
// settings.json (ux::prefs().walkthrough_2d_done / _3d_done). Help >
// Walkthrough runs it again any time.

#include <functional>
#include <string>

namespace ts::tombstone::editor::ux {

class Walkthrough {
 public:
  explicit Walkthrough(bool three_d) : three_d_(three_d) {}

  void start();
  // Skip: close. remember = "Don't show again" (also set by finishing).
  void stop(bool remember);
  void next();
  void back();
  bool active() const { return active_; }
  int step() const { return step_; }
  int step_count() const;
  bool seen() const;  // prefs say this editor's tour is done
  bool dont_show_again = true;  // the checkbox (ticked by default)

  // Per frame. Opens on first visit when the prefs say it hasn't been seen
  // (after a few frames, so the panels exist). show_panel(label) shows +
  // focuses the panel a step points at ("Hierarchy", "Vault"...).
  void frame(const std::function<void(const std::string&)>& show_panel);
  // Smokes and screenshot runs turn the first-run auto start off so a tour
  // card doesn't sit over what they test (Help > Walkthrough still works).
  static void set_autostart(bool on);
  static bool autostart();
  // A walkthrough opened automatically this session already.
  bool auto_started() const { return auto_started_; }

 private:
  bool three_d_ = false;
  bool active_ = false;
  bool auto_started_ = false;
  int step_ = 0;
  int frames_ = 0;
  int shown_step_ = -1;
};

}  // namespace ts::tombstone::editor::ux
