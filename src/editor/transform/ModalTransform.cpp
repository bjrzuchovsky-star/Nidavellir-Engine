#include "editor/transform/ModalTransform.h"

#include <cmath>
#include <cstdlib>

namespace ts {
namespace tombstone {
namespace editor {

const char* xform_op_label(XformOp op) {
  switch (op) {
    case XformOp::Move: return "Move";
    case XformOp::Rotate: return "Rotate";
    case XformOp::Scale: return "Scale";
    default: return "";
  }
}

const char* pivot_label(PivotMode p) {
  switch (p) {
    case PivotMode::Median: return "Median Point";
    case PivotMode::Individual: return "Individual Origins";
    case PivotMode::Active: return "Active Element";
    case PivotMode::Cursor: return "Cursor";
    case PivotMode::Bounds: return "Bounding Box Centre";
  }
  return "";
}

const char* xform_space_label(XformSpace s) {
  return s == XformSpace::Local ? "Local" : "Global";
}

void ModalTransform::begin(XformOp op, int dims, XformSpace space, float mx, float my) {
  dims_ = dims == 2 ? 2 : 3;
  space_ = space;
  switch_op(op, mx, my);
}

void ModalTransform::switch_op(XformOp op, float mx, float my) {
  op_ = op;
  lock_ = AxisLock{};
  lock_.space = space_;
  trackball_ = false;
  for (int i = 0; i < 3; ++i) {
    digits_[i].clear();
    neg_[i] = false;
  }
  comp_ = 0;
  sx_ = ex_ = lx_ = mx;
  sy_ = ey_ = ly_ = my;
  reset_sweep();
}

void ModalTransform::press_axis(int axis, bool shift) {
  if (axis < 0 || axis >= dims_) return;
  trackball_ = false;
  if (dims_ == 2 && shift) {
    // Two axes: "all but X" is Y.
    axis = 1 - axis;
    shift = false;
  }
  const XformSpace other = space_ == XformSpace::Global ? XformSpace::Local : XformSpace::Global;
  const bool same = shift ? lock_.plane == axis : lock_.axis == axis;
  AxisLock next;
  if (!same) {
    next.space = space_;
  } else if (lock_.space == space_) {
    next.space = other;  // second press: the other orientation
  } else {
    lock_ = AxisLock{};  // third press: free again
    lock_.space = space_;
    return;
  }
  if (shift) next.plane = axis;
  else next.axis = axis;
  lock_ = next;
}

bool ModalTransform::type_char(char c) {
  if (c == '-') {
    neg_[comp_] = !neg_[comp_];
    return true;
  }
  if (c == '.') {
    if (digits_[comp_].find('.') == std::string::npos) {
      if (digits_[comp_].empty()) digits_[comp_] = "0";
      digits_[comp_] += '.';
    }
    return true;
  }
  if (c >= '0' && c <= '9') {
    if (digits_[comp_].size() < 16) digits_[comp_] += c;
    return true;
  }
  return false;
}

void ModalTransform::backspace() {
  if (!digits_[comp_].empty()) {
    digits_[comp_].pop_back();
  } else if (neg_[comp_]) {
    neg_[comp_] = false;
  } else if (comp_ > 0) {
    --comp_;
  }
}

void ModalTransform::next_component() {
  comp_ = (comp_ + 1) % dims_;
}

bool ModalTransform::numeric() const {
  for (int i = 0; i < 3; ++i) {
    if (!digits_[i].empty()) return true;
  }
  return false;
}

float ModalTransform::value(int comp, float fallback) const {
  if (comp < 0 || comp > 2 || digits_[comp].empty()) return fallback;
  const float v = static_cast<float>(std::atof(digits_[comp].c_str()));
  return neg_[comp] ? -v : v;
}

std::string ModalTransform::typed_text(int comp) const {
  if (comp < 0 || comp > 2) return {};
  if (digits_[comp].empty() && !neg_[comp]) return {};
  return (neg_[comp] ? "-" : "") + digits_[comp];
}

void ModalTransform::mouse(float mx, float my, bool precise) {
  const float k = precise ? 0.1f : 1.0f;
  ex_ += (mx - lx_) * k;
  ey_ += (my - ly_) * k;
  lx_ = mx;
  ly_ = my;
}

std::string ModalTransform::constraint_text() const {
  static const char* kAxis[3] = {"X", "Y", "Z"};
  static const char* kPlane[3] = {"YZ", "XZ", "XY"};
  if (trackball_) return "trackball";
  if (lock_.axis >= 0) {
    return std::string("along ") + kAxis[lock_.axis] + " (" + xform_space_label(lock_.space) + ")";
  }
  if (lock_.plane >= 0) {
    if (op_ == XformOp::Rotate) {
      return std::string("about ") + kAxis[lock_.plane] + " (" + xform_space_label(lock_.space) + ")";
    }
    return std::string("plane ") + kPlane[lock_.plane] + " (" + xform_space_label(lock_.space) + ")";
  }
  return "free";
}

float ModalTransform::sweep(float cx, float cy) {
  const float a = std::atan2(ey_ - cy, ex_ - cx);
  if (!sweep_valid_) {
    // First call: measure from the start point.
    sweep_last_ = std::atan2(sy_ - cy, sx_ - cx);
    sweep_ = 0.0f;
    sweep_valid_ = true;
  }
  float d = a - sweep_last_;
  const float pi = 3.14159265358979f;
  while (d > pi) d -= 2.0f * pi;
  while (d < -pi) d += 2.0f * pi;
  sweep_ += d;
  sweep_last_ = a;
  return sweep_;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
