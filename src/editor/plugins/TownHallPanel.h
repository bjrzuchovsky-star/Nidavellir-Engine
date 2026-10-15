#pragma once

// Town Hall panel (posse plugin, 2D editor, Dusk theme): run the persistent
// world (ts_mmo) on this machine. Opens an in-editor server on its own
// thread (live map, riders, chat, admin console, bots) or launches
// ts_server --mmo as its own process; writes a sample mmo.json.

#include "mmo/LocalHost.h"

#include <deque>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

struct TownHallState {
  std::string project_dir;
  bool open = false;
  mmo::LocalHost host;
  int port = 24650;
  std::string db_path;  // "" = <project>/mmo/world.db
  int bots = 24;
  std::string zone;
  std::string selected;  // rider whose interest circle shows
  std::string console_line;
  std::deque<std::string> console_out;
  std::string announce;
  std::string status;
};

void draw_town_hall_panel(TownHallState& s);
// Open / close the in-editor Town Hall (the panel's buttons, the smokes).
bool town_hall_open(TownHallState& s, std::string* error);
void town_hall_close(TownHallState& s);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
