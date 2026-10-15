#pragma once

// Editor panels of the ts.profiler plugin (Dusk theme), installed through
// ProfilerPlugin::set_editor_hook by register_editor_builtin_plugins:
//   Assay Office     record / capture / load capture, frame-time bars
//                    (click a bar to inspect it), a timeline (flame) view
//                    of the selected frame, zone stats table.
//   Render & Memory  GPU pass timings, draw calls, triangles, asset counts,
//                    memory, with sparklines.
//   Net              posse round trip, bandwidth, snapshots, corrections.
// Plus a console command line (Console::shared()) any panel can draw.

#include "profiler/Profiler.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

struct AssayPanelState {
  bool live = true;                       // false = viewing a loaded capture
  std::vector<profiler::FrameRecord> frames;  // snapshot shown
  std::string loaded_path;
  int selected = -1;                      // index into frames
  float zoom = 1.0f;
  std::string capture_name;
  std::string status;
  std::vector<std::string> captures;      // files in the capture dir
  double scanned = -1.0;
  int stats_window = 120;
};
void draw_assay_panel(AssayPanelState& s);

void draw_render_memory_panel();
void draw_net_panel();

// Command line for Console::shared(): returns true when a line ran and
// fills `echo` / `output` (the caller logs them where it likes).
struct ConsoleLineState {
  std::string line;
  std::vector<std::string> history;
  int cursor = -1;
};
bool draw_console_line(ConsoleLineState& s, std::string* echo, std::string* output);

// Pure helpers (smoke-tested).
// Sum of counter `name` over the frames that have it, and how many did.
double counter_series(const std::vector<profiler::FrameRecord>& frames, const std::string& name,
                      std::vector<float>* out);
std::vector<std::string> counter_names(const std::vector<profiler::FrameRecord>& frames,
                                       const std::string& prefix);

void install_profiler_panels();

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
