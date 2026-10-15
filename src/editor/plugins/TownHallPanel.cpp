#include "editor/plugins/TownHallPanel.h"

#include "editor/launch/GameLauncher.h"
#include "editor/ui/Theme.h"
#include "mmo/MmoWorld.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

int resize_cb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(d->UserData);
    str->resize(static_cast<std::size_t>(d->BufTextLen));
    d->Buf = str->data();
  }
  return 0;
}

bool input_str(const char* label, std::string* s, ImGuiInputTextFlags extra = 0, const char* hint = nullptr) {
  const ImGuiInputTextFlags f = ImGuiInputTextFlags_CallbackResize | extra;
  if (hint) return ImGui::InputTextWithHint(label, hint, s->data(), s->capacity() + 1, f, resize_cb, s);
  return ImGui::InputText(label, s->data(), s->capacity() + 1, f, resize_cb, s);
}

void console_print(TownHallState& s, const std::string& text) {
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      s.console_out.push_back(line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty()) s.console_out.push_back(line);
  while (s.console_out.size() > 400) s.console_out.pop_front();
}

ImVec4 channel_color(const std::string& ch) {
  if (ch == "admin" || ch == "system") return theme::Danger();
  if (ch == "whisper") return theme::Copper();
  if (ch == "zone") return theme::Sand();
  if (ch == "world") return theme::Accent();
  if (ch == "say") return theme::Text();
  return theme::Success();
}

struct Snapshot {
  mmo::MmoStats stats;
  std::vector<mmo::CharacterView> riders;
  std::vector<mmo::ZoneEntityView> ents;
  std::vector<std::string> zones;
  mmo::ZoneGeometry geo;
  std::vector<mmo::ChatLine> chat;
  float radius = 640.0f;
  int port = 0;
  int bots_in = 0;
};

Snapshot take(TownHallState& s) {
  Snapshot snap;
  s.host.with([&](mmo::MmoServer& server, mmo::BotSwarm* bots) {
    snap.stats = server.stats();
    snap.riders = server.characters();
    snap.zones = server.zone_ids();
    if (s.zone.empty() && !snap.zones.empty()) s.zone = snap.zones.front();
    if (const mmo::ZoneDef* z = server.zone_def(s.zone)) snap.geo = z->geo;
    snap.ents = server.zone_entities(s.zone);
    const auto& log = server.chat_log();
    const std::size_t from = log.size() > 120 ? log.size() - 120 : 0;
    for (std::size_t i = from; i < log.size(); ++i) snap.chat.push_back(log[i]);
    snap.radius = server.world_config().interest_radius;
    snap.port = server.port();
    snap.bots_in = bots ? bots->in_world() : 0;
  });
  return snap;
}

void draw_map(TownHallState& s, const Snapshot& snap) {
  ImGui::SetNextItemWidth(160);
  if (ImGui::BeginCombo("Zone", s.zone.c_str())) {
    for (const std::string& z : snap.zones)
      if (ImGui::Selectable(z.c_str(), z == s.zone)) s.zone = z;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s  |  click a rider to see what it sees", snap.geo.name.c_str());
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 size(std::max(200.0f, avail.x), std::max(200.0f, avail.y - 4));
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##townmap", size);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), theme::U32(theme::CanvasBg()));
  const runtime::Aabb& b = snap.geo.bounds;
  const float ww = std::max(1.0f, b.x1 - b.x0), wh = std::max(1.0f, b.y1 - b.y0);
  const float scale = std::min((size.x - 16) / ww, (size.y - 16) / wh);
  const ImVec2 o(p0.x + (size.x - ww * scale) * 0.5f, p0.y + (size.y - wh * scale) * 0.5f);
  auto at = [&](float x, float y) { return ImVec2(o.x + (x - b.x0) * scale, o.y + (y - b.y0) * scale); };
  dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
  dl->AddRect(at(b.x0, b.y0), at(b.x1, b.y1), theme::U32(theme::Border()), 0, 0, 2.0f);
  // Grid every 500 px.
  for (float gx = std::ceil(b.x0 / 500) * 500; gx < b.x1; gx += 500)
    dl->AddLine(at(gx, b.y0), at(gx, b.y1), theme::U32(theme::GridLine(), 0.5f));
  for (float gy = std::ceil(b.y0 / 500) * 500; gy < b.y1; gy += 500)
    dl->AddLine(at(b.x0, gy), at(b.x1, gy), theme::U32(theme::GridLine(), 0.5f));
  for (const auto& bl : snap.geo.blockers)
    dl->AddRectFilled(at(bl.x0, bl.y0), at(bl.x1, bl.y1), theme::U32(theme::CopperMuted()));
  for (const auto& p : snap.geo.portals) {
    dl->AddRect(at(p.rect.x0, p.rect.y0), at(p.rect.x1, p.rect.y1), theme::U32(theme::Accent()), 0, 0, 2.0f);
    dl->AddText(ImVec2(at(p.rect.x0, p.rect.y0).x - 30, at(p.rect.x0, p.rect.y0).y - 16),
                theme::U32(theme::Accent()), ("to " + p.to).c_str());
  }
  // Selected rider's interest radius.
  for (const auto& r : snap.riders)
    if (r.name == s.selected && r.zone == s.zone) {
      dl->AddCircleFilled(at(r.x, r.y), snap.radius * scale, theme::U32(theme::Accent(), 0.08f), 64);
      dl->AddCircle(at(r.x, r.y), snap.radius * scale, theme::U32(theme::Accent(), 0.7f), 64, 1.5f);
    }
  for (const auto& e : snap.ents) {
    if (e.kind == mmo::EntityKind::Character) continue;
    const ImVec2 c = at(e.x, e.y);
    if (e.kind == mmo::EntityKind::Item) dl->AddCircleFilled(c, 4.0f, theme::U32(theme::Sand()));
    else dl->AddRectFilled(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), theme::U32(theme::TextMuted()));
  }
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const mmo::CharacterView* nearest = nullptr;
  float best = 12.0f * 12.0f;
  for (const auto& r : snap.riders) {
    if (r.zone != s.zone) continue;
    const ImVec2 c = at(r.x, r.y);
    const bool sel = r.name == s.selected;
    const ImVec4 col = !r.online ? theme::TextMuted() : r.admin ? theme::Danger() : theme::Accent();
    dl->AddCircleFilled(c, sel ? 6.0f : 4.0f, theme::U32(col, r.online ? 1.0f : 0.5f));
    if (sel || scale > 0.5f) dl->AddText(ImVec2(c.x + 7, c.y - 7), theme::U32(theme::Text()), r.name.c_str());
    const float dx = c.x - mouse.x, dy = c.y - mouse.y;
    if (dx * dx + dy * dy < best) {
      best = dx * dx + dy * dy;
      nearest = &r;
    }
  }
  dl->PopClipRect();
  if (hovered && nearest) {
    ImGui::SetTooltip("%s (%s)\n%.0f, %.0f  sees %d  rtt %u ms%s", nearest->name.c_str(), nearest->account.c_str(),
                      nearest->x, nearest->y, nearest->visible, nearest->rtt_ms,
                      nearest->online ? "" : "\nlink-dead");
    if (ImGui::IsMouseClicked(0)) s.selected = nearest->name;
  }
}

void draw_riders(TownHallState& s, const Snapshot& snap) {
  if (ImGui::BeginTable("##riders", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"Rider", "Account", "Zone", "Position", "RTT", "Sees", "Strikes", "Items", ""})
      ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (const auto& r : snap.riders) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable(r.name.c_str(), r.name == s.selected, ImGuiSelectableFlags_SpanAllColumns |
                                                                       ImGuiSelectableFlags_AllowOverlap)) {
        s.selected = r.name;
        s.zone = r.zone;
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.account.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.zone.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%.0f, %.0f", r.x, r.y);
      ImGui::TableNextColumn();
      if (r.online) ImGui::Text("%u ms", r.rtt_ms);
      else ImGui::TextDisabled("link-dead");
      ImGui::TableNextColumn();
      ImGui::Text("%d", r.visible);
      ImGui::TableNextColumn();
      if (r.strikes > 3) ImGui::TextColored(theme::Danger(), "%.1f", r.strikes);
      else ImGui::Text("%.1f", r.strikes);
      ImGui::TableNextColumn();
      ImGui::Text("%d", r.items);
      ImGui::TableNextColumn();
      ImGui::PushID(r.name.c_str());
      if (ImGui::SmallButton("Kick")) console_print(s, s.host.admin("kick " + r.name));
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

void draw_chat(TownHallState& s, const Snapshot& snap) {
  ImGui::BeginChild("##chatlog", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
  for (const auto& l : snap.chat) {
    ImGui::PushStyleColor(ImGuiCol_Text, channel_color(l.channel));
    if (l.channel == "whisper")
      ImGui::TextWrapped("[%s] %s -> %s: %s", l.channel.c_str(), l.from.c_str(), l.to.c_str(), l.text.c_str());
    else
      ImGui::TextWrapped("[%s%s%s] %s: %s", l.channel.c_str(), l.zone.empty() ? "" : " @ ", l.zone.c_str(),
                         l.from.c_str(), l.text.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::SetNextItemWidth(-1);
  if (input_str("##announce", &s.announce, ImGuiInputTextFlags_EnterReturnsTrue, "announce to everyone...") &&
      !s.announce.empty()) {
    console_print(s, s.host.admin("announce " + s.announce));
    s.announce.clear();
    ImGui::SetKeyboardFocusHere(-1);
  }
}

void draw_console(TownHallState& s) {
  ImGui::BeginChild("##console", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
  for (const auto& l : s.console_out) {
    if (!l.empty() && l[0] == '>') ImGui::TextColored(theme::Accent(), "%s", l.c_str());
    else ImGui::TextUnformatted(l.c_str());
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::SetNextItemWidth(-1);
  if (input_str("##admin", &s.console_line, ImGuiInputTextFlags_EnterReturnsTrue,
                "admin command (help, who, stats, kick, ban, tp, give, announce, save...)") &&
      !s.console_line.empty()) {
    console_print(s, "> " + s.console_line);
    console_print(s, s.host.admin(s.console_line));
    s.console_line.clear();
    ImGui::SetKeyboardFocusHere(-1);
  }
}

}  // namespace

bool town_hall_open(TownHallState& s, std::string* error) {
  mmo::MmoServerConfig cfg;
  cfg.project_dir = s.project_dir;
  cfg.db_path = s.db_path;
  cfg.port = static_cast<std::uint16_t>(std::clamp(s.port, 0, 65535));
  // Bots log in by the dozen: cheaper hashes on the editor's own server.
  cfg.argon2_kib = 1024;
  std::string err;
  if (!s.host.start(std::move(cfg), &err)) {
    if (error) *error = err;
    s.status = "Could not open: " + err;
    return false;
  }
  s.host.with([&](mmo::MmoServer& server, mmo::BotSwarm*) {
    for (const std::string& l : server.take_log()) console_print(s, l);
  });
  s.status.clear();
  return true;
}

void town_hall_close(TownHallState& s) {
  s.host.stop();
  s.status = "Closed; characters saved.";
}

void draw_town_hall_panel(TownHallState& s) {
  std::error_code ec;
  const bool has_cfg = fs::exists(fs::path(s.project_dir) / "mmo.json", ec);
  if (!s.host.running()) {
    ImGui::TextColored(theme::Accent(), "Town Hall: the persistent world");
    ImGui::TextWrapped("Accounts, saved riders (SQLite), zones with interest management, chat channels and an admin "
                       "console, on this machine. %s",
                       has_cfg ? "This project has an mmo.json." : "No mmo.json yet: the scene is one zone.");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("UDP port", &s.port);
    ImGui::SetNextItemWidth(-1);
    input_str("##db", &s.db_path, 0, "database (default <project>/mmo/world.db)");
    if (ImGui::Button("Open Town Hall here")) {
      std::string err;
      town_hall_open(s, &err);
    }
    ImGui::SameLine();
    if (ImGui::Button("Launch ts_server --mmo")) {
      const std::string exe = launcher::find_server_executable();
      std::string err;
      if (exe.empty()) s.status = "ts_server is not built next to the editor.";
      else if (launcher::spawn_mmo_server(exe, s.project_dir, s.port, &err))
        s.status = "Launched " + exe + " --mmo on UDP " + std::to_string(s.port) + " (its own console takes admin commands).";
      else s.status = err;
    }
    if (!has_cfg) {
      ImGui::SameLine();
      if (ImGui::Button("Write sample mmo.json")) {
        std::string err;
        s.status = mmo::write_sample_world(s.project_dir, &err) ? "Wrote mmo.json: Frontier Town + Lucky Strike Mine."
                                                                 : err;
      }
    }
    if (!s.status.empty()) ImGui::TextDisabled("%s", s.status.c_str());
    return;
  }
  const Snapshot snap = take(s);
  s.host.with([&](mmo::MmoServer& server, mmo::BotSwarm*) {
    for (const std::string& l : server.take_log()) console_print(s, l);
  });
  ImGui::TextColored(theme::Success(), "Open on UDP %d", snap.port);
  ImGui::SameLine();
  ImGui::TextDisabled("| %d online, %d link-dead, %d accounts | tick %.2f ms | %.1f KB/s out | sees %.1f of %d",
                      snap.stats.online, snap.stats.linkdead, snap.stats.accounts, snap.stats.tick_ms_avg,
                      snap.stats.kbps_out, snap.stats.avg_visible, snap.stats.world_entities);
  if (ImGui::Button("Close")) {
    town_hall_close(s);
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Save now")) console_print(s, s.host.admin("save"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160);
  ImGui::SliderInt("##bots", &s.bots, 0, 256, "%d bots");
  ImGui::SameLine();
  if (ImGui::Button(s.host.bots() == s.bots ? "Bots riding" : "Send bots")) s.host.set_bots(s.bots);
  if (s.host.bots() > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("%d in the world", snap.bots_in);
  }
  if (ImGui::BeginTabBar("##townhall")) {
    if (ImGui::BeginTabItem("Map")) {
      draw_map(s, snap);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Riders")) {
      draw_riders(s, snap);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Chat")) {
      draw_chat(s, snap);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Console")) {
      draw_console(s);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
