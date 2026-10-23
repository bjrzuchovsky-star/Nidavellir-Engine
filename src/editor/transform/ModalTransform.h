#pragma once

// Blender-style modal transforms shared by the 2D and 3D editors
// (docs/transforms.md). Pure state: the editors feed it keys and the mouse
// and read back the constraint, typed numbers and the header line; each
// editor turns that into entity edits (Editor2DScreen_Transform.cpp,
// Editor3DScreen_Transform.cpp). The smokes drive the same calls.
//
//   G / R / S          start move / rotate / scale from the mouse
//   X / Y / Z          lock to that axis; again = the other orientation
//                      (Global <-> Local); a third time = free
//   Shift+X / Y / Z    every axis but that one (a plane)
//   0-9 . - Backspace  exact values (Tab = next component for moves)
//   Ctrl (held)        toggles snapping; Shift (held) = precision (x0.1)
//   Enter / LMB        confirm (one undo step); Esc / RMB cancel

#include <string>

namespace ts {
namespace tombstone {
namespace editor {

enum class XformOp { None, Move, Rotate, Scale };
// Where rotate / scale turn about (Blender's Pivot Point).
enum class PivotMode { Median, Individual, Active, Cursor, Bounds };
// Transform orientation of the gizmo and the first axis press.
enum class XformSpace { Global, Local };

const char* xform_op_label(XformOp op);      // "Move", "Rotate", "Scale"
const char* pivot_label(PivotMode p);        // "Median Point", ...
const char* xform_space_label(XformSpace s);  // "Global" / "Local"
inline constexpr int kPivotModeCount = 5;

// Axis constraint. axis >= 0: only that axis; plane >= 0: every axis but
// that one. space: the frame the axes are in.
struct AxisLock {
  int axis = -1;
  int plane = -1;
  XformSpace space = XformSpace::Global;
  bool any() const { return axis >= 0 || plane >= 0; }
  // Whether axis a moves / scales under this lock.
  bool uses(int a) const {
    return axis >= 0 ? a == axis : (plane >= 0 ? a != plane : true);
  }
};

class ModalTransform {
 public:
  // dims: 2 (X / Y only; Z keys ignored) or 3. space: the editor's
  // orientation setting (the first axis press uses it).
  void begin(XformOp op, int dims, XformSpace space, float mx, float my);
  // Same session, another operation (G / R / S pressed mid-transform):
  // the constraint and the typed numbers start over.
  void switch_op(XformOp op, float mx, float my);
  void end() { op_ = XformOp::None; }
  bool active() const { return op_ != XformOp::None; }
  XformOp op() const { return op_; }
  int dims() const { return dims_; }

  // Constraint keys. shift = plane (exclude the axis).
  void press_axis(int axis, bool shift);
  void set_lock(const AxisLock& lock) { lock_ = lock; }
  const AxisLock& lock() const { return lock_; }
  // R again during a 3D rotate: free trackball rotation.
  void toggle_trackball() { trackball_ = !trackball_; lock_ = AxisLock{}; }
  bool trackball() const { return trackball_; }

  // Numeric input. type_char takes '0'-'9', '.', '-' (minus negates the
  // current component); false for anything else.
  bool type_char(char c);
  void backspace();
  void next_component();  // Tab (moves: X -> Y -> Z)
  int component() const { return comp_; }
  // Any digits typed (values then replace the mouse).
  bool numeric() const;
  bool has_digits(int comp) const { return !digits_[comp].empty(); }
  // Typed value of a component; `fallback` when it has no digits.
  float value(int comp, float fallback) const;
  // Minus typed with no digits anywhere: the mouse value is negated.
  bool negate_mouse() const { return !numeric() && neg_[comp_]; }
  std::string typed_text(int comp) const;  // "-2.5" ("" when nothing)

  // Mouse with precision: deltas are scaled by 0.1 while `precise` (Shift)
  // is held, so the effective point creeps.
  void mouse(float mx, float my, bool precise);
  float start_x() const { return sx_; }
  float start_y() const { return sy_; }
  float eff_x() const { return ex_; }
  float eff_y() const { return ey_; }

  // Snapping this frame: the editor's setting, flipped while Ctrl is held.
  static bool snap_now(bool setting, bool ctrl_held) { return setting != ctrl_held; }

  // Constraint words for the header ("along X (Local)", "plane YZ", ...).
  std::string constraint_text() const;

  // Accumulated screen angle (radians) around a centre, unwrapped across
  // frames so turns past 180 degrees keep counting.
  float sweep(float cx, float cy);
  void reset_sweep() { sweep_valid_ = false; sweep_ = 0.0f; }

 private:
  XformOp op_ = XformOp::None;
  int dims_ = 3;
  XformSpace space_ = XformSpace::Global;
  AxisLock lock_;
  bool trackball_ = false;
  std::string digits_[3];
  bool neg_[3] = {false, false, false};
  int comp_ = 0;
  float sx_ = 0, sy_ = 0, ex_ = 0, ey_ = 0, lx_ = 0, ly_ = 0;
  bool sweep_valid_ = false;
  float sweep_ = 0.0f, sweep_last_ = 0.0f;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
