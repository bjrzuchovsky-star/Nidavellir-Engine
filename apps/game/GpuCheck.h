#pragma once

// OpenGL 3.3 gate for shipped games: `ts_game --check-gpu [--quiet]`
// (installers run it after copying files) and a friendly message box when
// the game window cannot get a GL context.
#include <string>

namespace ts {
namespace tombstone {
namespace game {

// 0 = an OpenGL 3.3 core context opened; 3 = it did not. Without --quiet a
// failure also shows the friendly box.
int run_check_gpu(bool quiet);

// Prints and (Windows) shows "<game> needs a graphics card with OpenGL
// 3.3..." with the driver's reason. TS_NO_DIALOGS=1 keeps it on stderr.
void show_gpu_error(const std::string& game_name, const std::string& detail);

// The last GLFW error text ("" when none).
std::string last_glfw_error();

}  // namespace game
}  // namespace tombstone
}  // namespace ts
