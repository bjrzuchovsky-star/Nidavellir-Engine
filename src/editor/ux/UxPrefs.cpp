#include "editor/ux/UxPrefs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ts::tombstone::editor::ux {

Prefs& prefs() {
  static Prefs p;
  return p;
}

std::string format_delay(float seconds) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", seconds);
  return buf;
}

void import_from(const Settings& s) {
  Prefs& p = prefs();
  p.tooltips = s.tooltips_enabled;
  const float d = static_cast<float>(std::atof(s.tooltip_delay.c_str()));
  p.tooltip_delay = s.tooltip_delay.empty() ? 0.5f : std::clamp(d, 0.0f, 3.0f);
  p.walkthrough_2d_done = s.walkthrough_2d_done;
  p.walkthrough_3d_done = s.walkthrough_3d_done;
  p.recent.clear();
  std::istringstream in(s.recent_items);
  std::string line;
  while (std::getline(in, line) && p.recent.size() < kMaxRecent) {
    const std::size_t a = line.find('\t');
    if (a == std::string::npos) continue;
    const std::size_t b = line.find('\t', a + 1);
    RecentItem r;
    r.kind = line.substr(0, a);
    r.path = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    r.label = b == std::string::npos ? r.path : line.substr(b + 1);
    if (!r.kind.empty() && !r.path.empty()) p.recent.push_back(std::move(r));
  }
}

void export_to(Settings* s) {
  if (!s) return;
  const Prefs& p = prefs();
  s->tooltips_enabled = p.tooltips;
  s->tooltip_delay = format_delay(p.tooltip_delay);
  s->walkthrough_2d_done = p.walkthrough_2d_done;
  s->walkthrough_3d_done = p.walkthrough_3d_done;
  std::string out;
  for (const RecentItem& r : p.recent) {
    if (!out.empty()) out += '\n';
    out += r.kind + '\t' + r.path + '\t' + r.label;
  }
  s->recent_items = out;
}

bool persist(std::string* error) {
  std::string err;
  Settings s = SettingsStore::load(&err);
  export_to(&s);
  return SettingsStore::save(s, error);
}

void add_recent(const std::string& kind, const std::string& path, const std::string& label) {
  if (kind.empty() || path.empty()) return;
  std::vector<RecentItem>& r = prefs().recent;
  r.erase(std::remove_if(r.begin(), r.end(),
                         [&](const RecentItem& x) { return x.kind == kind && x.path == path; }),
          r.end());
  r.insert(r.begin(), RecentItem{kind, path, label.empty() ? path : label});
  if (r.size() > kMaxRecent) r.resize(kMaxRecent);
  persist();
}

}  // namespace ts::tombstone::editor::ux
