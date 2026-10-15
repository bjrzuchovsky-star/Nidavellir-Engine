#pragma once

// Editor panel of the ts.weather plugin ("Weather"), installed through
// WeatherPlugin::set_editor_hook by register_editor_builtin_plugins: edits
// weather.json (start kind, intensity, cycle, wind, lightning, time-of-day
// link), previews the mix in the panel, and drives the live weather during
// Play (kind buttons, strike, time of day).

namespace ts {
namespace tombstone {
namespace editor {

void install_weather_panels();

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
