#include "editor/team/TeamPanel.h"

#include "editor/ux/Actions.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace team_ui {

namespace {

ImVec4 colour_vec(const std::string& hex) {
  const ImU32 c = colour_u32(hex);
  return ImGui::ColorConvertU32ToFloat4(c);
}

void default_user(TeamUiState& ui) {
  if (ui.user[0]) return;
  const char* u = std::getenv("USERNAME");
  if (!u || !*u) u = std::getenv("USER");
  std::snprintf(ui.user, sizeof(ui.user), "%s", u && *u ? u : "Rider");
}

void swatch(const std::string& hex) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 4, p.y + h - 2),
                                            colour_u32(hex), 3.0f);
  ImGui::Dummy(ImVec2(h - 2, h));
  ImGui::SameLine();
}

void host_now(TeamSession& t) {
  TeamHostSettings s;
  s.port = static_cast<std::uint16_t>(std::clamp(t.ui.host_port, 1, 65535));
  s.pass = t.ui.pass;
  s.user = t.ui.user;
  s.vault_url = t.ui.vault_url;
  s.vault_key = t.ui.key;
  std::string err;
  t.ui.last_error.clear();
  if (!t.host(s, &err)) t.ui.last_error = "Could not host: " + err;
}

void join_now(TeamSession& t) {
  TeamJoinSettings s;
  std::string host = t.ui.host_addr;
  s.port = static_cast<std::uint16_t>(std::clamp(t.ui.join_port, 1, 65535));
  // "host:port" in the address box wins over the port field.
  const auto colon = host.rfind(':');
  if (colon != std::string::npos && host.find(']') == std::string::npos &&
      host.find(':') == colon) {
    s.port = static_cast<std::uint16_t>(std::clamp(std::atoi(host.c_str() + colon + 1), 1, 65535));
    host = host.substr(0, colon);
  }
  s.host = host;
  s.key = t.ui.key;
  s.pass = t.ui.pass;
  s.user = t.ui.user;
  s.vault_url = t.ui.vault_url;
  std::string err;
  t.ui.last_error.clear();
  if (!t.join(s, &err)) t.ui.last_error = "Could not ride out: " + err;
}

}  // namespace

unsigned int colour_u32(const std::string& hex, int alpha) {
  unsigned r = 200, g = 160, b = 90;
  if (hex.size() == 7 && hex[0] == '#') std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
  return IM_COL32(r, g, b, std::clamp(alpha, 0, 255));
}

void draw_menu(TeamSession& t, bool* show_panel) {
  default_user(t.ui);
  const bool active = t.active();
  if (ux::MenuItem("Host from my editor...", nullptr, false, !active)) {
    t.ui.mode = 1;
    *show_panel = true;
  }
  if (ux::MenuItem("Join a posse...", nullptr, false, !active)) {
    t.ui.mode = 0;
    *show_panel = true;
  }
  if (ux::MenuItem("Users", nullptr, false, true)) *show_panel = true;
  ImGui::Separator();
  if (ux::MenuItem("Leave", nullptr, false, active)) t.leave();
}

void draw_panel(TeamSession& t, bool* open, const char* window_id) {
  default_user(t.ui);
  // First open: over the top of the viewport, clear of Hierarchy / Inspector.
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(std::max(0.0f, ds.x * 0.27f), 56.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(340, 210), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(window_id, open)) {
    ImGui::End();
    return;
  }
  team::TeamClient& c = t.client();
  theme::SectionHeader("Team Claim", "ride this claim with your posse");
  ImGui::TextWrapped("%s", t.status_line().c_str());
  if (!c.warning().empty()) ImGui::TextColored(theme::Warning(), "%s", c.warning().c_str());
  if (!t.ui.last_error.empty()) ImGui::TextColored(theme::Danger(), "%s", t.ui.last_error.c_str());

  if (!t.active()) {
    ImGui::Spacing();
    ImGui::RadioButton("Join", &t.ui.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Host from my editor", &t.ui.mode, 1);
    ImGui::InputText("Your name", t.ui.user, sizeof(t.ui.user));
    if (t.ui.mode == 0) {
      ImGui::InputText("Address", t.ui.host_addr, sizeof(t.ui.host_addr));
      ImGui::InputInt("Port (UDP)", &t.ui.join_port);
      ImGui::InputText("Vault API key", t.ui.key, sizeof(t.ui.key), ImGuiInputTextFlags_Password);
      ImGui::InputText("Passcode", t.ui.pass, sizeof(t.ui.pass), ImGuiInputTextFlags_Password);
      ImGui::InputText("Vault URL (optional)", t.ui.vault_url, sizeof(t.ui.vault_url));
      ImGui::TextDisabled("ts_vault --team: use your Vault key. LAN host: the passcode, if any.");
      ImGui::TextDisabled("Files under assets/ come to you on their own; leave the URL empty");
      ImGui::TextDisabled("unless the host can't say where they live (older TombStone).");
      ImGui::TextDisabled("Joining swaps this claim's scene for the posse's.");
      const bool back = t.has_last_ride() && !t.last_ride_hosted();
      if (back) ImGui::TextDisabled("Filled in from your last ride (keys and passcodes are never saved).");
      if (ImGui::Button(back ? "Ride back in (Join)" : "Ride out (Join)")) join_now(t);
    } else {
      ImGui::InputInt("Port (UDP)", &t.ui.host_port);
      ImGui::InputText("Passcode (optional)", t.ui.pass, sizeof(t.ui.pass),
                       ImGuiInputTextFlags_Password);
      ImGui::InputText("Vault URL (optional)", t.ui.vault_url, sizeof(t.ui.vault_url));
      ImGui::InputText("Vault API key", t.ui.key, sizeof(t.ui.key), ImGuiInputTextFlags_Password);
      ImGui::TextDisabled("Others join with this PC's address and the port.");
      ImGui::TextDisabled("No Vault URL: the posse's files travel over the session itself.");
      ImGui::TextDisabled("The session is kept in .tombstone/team/host: hosting again picks it up.");
      const bool back = t.has_last_ride() && t.last_ride_hosted();
      if (ImGui::Button(back ? "Open the claim again (Host)" : "Open the claim (Host)")) host_now(t);
    }
    ImGui::End();
    return;
  }

  if (c.joined()) {
    if (t.hosting() && t.server()) {
      ImGui::Text("Hosting on UDP %u", static_cast<unsigned>(t.server()->port()));
    }
    ImGui::Text("Edits: %llu sent, seq %llu", static_cast<unsigned long long>(t.batches_sent()),
                static_cast<unsigned long long>(c.seq()));
    if (!t.files_error().empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
      ImGui::TextWrapped("%s", t.files_error().c_str());
      ImGui::PopStyleColor();
    }
    if (t.has_asset_backend()) {
      if (!t.files_route().empty()) ImGui::TextWrapped("Files travel %s.", t.files_route().c_str());
      ImGui::Text("Files: %llu sent, %llu fetched, %llu removed",
                  static_cast<unsigned long long>(t.assets_sent()),
                  static_cast<unsigned long long>(t.assets_fetched()),
                  static_cast<unsigned long long>(t.assets_removed()));
      const TeamAssetProgress p = t.asset_progress();
      if (p.running && p.files_total > 0) {
        // Freight on the trail: the editor keeps working while it moves.
        const float frac = p.bytes_total ? static_cast<float>(static_cast<double>(p.bytes_done) /
                                                              static_cast<double>(p.bytes_total))
                                         : static_cast<float>(p.files_done) / static_cast<float>(p.files_total);
        char over[96];
        std::snprintf(over, sizeof(over), "%d / %d files, %.1f / %.1f MB", p.files_done, p.files_total,
                      static_cast<double>(p.bytes_done) / 1048576.0,
                      static_cast<double>(p.bytes_total) / 1048576.0);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), over);
        ImGui::TextDisabled("%s %s", p.stage.c_str(), p.current.c_str());
      } else if (p.running) {
        ImGui::TextDisabled("Checking the freight%s%s", p.current.empty() ? "" : ": ", p.current.c_str());
      }
    }
    ImGui::Spacing();
    theme::SectionHeader("Posse");
    // Me first.
    swatch(c.my_colour());
    ImGui::Text("%s (you)", c.my_name().c_str());
    std::size_t mine = 0;
    for (const auto& [k, who] : c.locks()) mine += who == c.me();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lock(s)%s", mine, c.can_write() ? "" : ", viewer");
    if (c.can_write()) {
      const std::vector<std::uint64_t> held = t.selection_held_by_others();
      if (!held.empty()) {
        char label[64];
        std::snprintf(label, sizeof label, "Take over (%zu)", held.size());
        if (ImGui::SmallButton(label)) t.take_over(held);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Someone else holds what you have selected. You can take it over once they've\n"
                            "left it alone for 15 seconds (no edits, no dragging).");
        }
      }
    }
    for (const auto& [id, m] : c.members()) {
      swatch(m.colour);
      ImGui::TextColored(colour_vec(m.colour), "%s", m.name.c_str());
      std::size_t n = 0;
      for (const auto& [k, who] : c.locks()) n += who == id;
      ImGui::SameLine();
      ImGui::TextDisabled("in %s, %zu lock(s)%s", m.scene.empty() ? "?" : m.scene.c_str(), n,
                          m.can_write ? "" : ", viewer");
    }
  }
  ImGui::Spacing();
  if (ImGui::Button(t.hosting() ? "Close the claim (Leave)" : "Ride off (Leave)")) t.leave();
  ImGui::End();
}

bool draw_lock_badge(const TeamSession* t, std::uint64_t id) {
  if (!t) return false;
  const team::RemoteMember* m = t->holder_member(id);
  if (!m) return false;
  ImGui::SameLine();
  ImGui::TextColored(colour_vec(m->colour), "[locked: %s]", m->name.c_str());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s has this selected. Edits wait for them.", m->name.c_str());
  return true;
}

bool draw_lock_badge_in(const TeamSession* t, std::uint64_t id, const ImVec2& mn,
                        const ImVec2& mx, float pad) {
  if (!t) return false;
  const team::RemoteMember* m = t->holder_member(id);
  if (!m) return false;
  const std::string text = "[locked: " + m->name + "]";
  const float tw = ImGui::CalcTextSize(text.c_str()).x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRect(mn, mx, colour_u32(m->colour, 200), 2.0f, 0, 1.5f);
  dl->AddText(ImVec2(mx.x - tw - pad - 6.0f, mn.y + ImGui::GetStyle().FramePadding.y),
              colour_u32(m->colour), text.c_str());
  if (ImGui::IsMouseHoveringRect(mn, mx)) {
    ImGui::SetTooltip("%s has this selected. Your edits to it are turned back.", m->name.c_str());
  }
  return true;
}

void draw_presence(TeamSession& t, ImDrawList* draw,
                   const std::function<bool(const float* xyz, ImVec2* out)>& project,
                   const std::function<bool(std::uint64_t id, ImVec2* mn, ImVec2* mx)>& bounds) {
  if (!draw || !t.joined()) return;
  const std::string scene = t.binding().scene_key();
  for (const auto& [id, m] : t.client().members()) {
    if (!m.has_presence || m.presence.str("scene") != scene) continue;
    const ImU32 col = colour_u32(m.colour);
    // Selection outlines.
    for (const team::Value& sv : m.presence["sel"].items()) {
      ImVec2 mn, mx;
      if (!bounds(team::id_of(sv), &mn, &mx)) continue;
      draw->AddRect(ImVec2(mn.x - 3, mn.y - 3), ImVec2(mx.x + 3, mx.y + 3), col, 3.0f, 0, 2.0f);
      draw->AddText(ImVec2(mn.x - 3, mn.y - 18), col, m.name.c_str());
    }
    // Cursor: an arrow with the name.
    float xyz[3];
    if (!m.presence.floats("cursor", xyz, 3)) continue;
    ImVec2 p;
    if (!project(xyz, &p)) continue;
    draw->AddTriangleFilled(p, ImVec2(p.x + 4, p.y + 15), ImVec2(p.x + 11, p.y + 10), col);
    draw->AddTriangle(p, ImVec2(p.x + 4, p.y + 15), ImVec2(p.x + 11, p.y + 10), IM_COL32(20, 14, 10, 220));
    const ImVec2 ts = ImGui::CalcTextSize(m.name.c_str());
    const ImVec2 tp(p.x + 12, p.y + 12);
    draw->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(tp.x + ts.x + 3, tp.y + ts.y + 1),
                        colour_u32(m.colour, 210), 3.0f);
    draw->AddText(tp, IM_COL32(24, 16, 10, 255), m.name.c_str());
  }
}

void draw_status_chip(TeamSession* t) {
  if (!t || !t->joined()) return;
  ImGui::SameLine();
  ImGui::TextColored(colour_vec(t->client().my_colour()), "Posse of %zu",
                     t->client().members().size() + 1);
  const TeamAssetProgress p = t->asset_progress();
  if (p.running && p.files_total > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("| freight %d/%d", p.files_done, p.files_total);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s", p.stage.c_str(), p.current.c_str());
  }
}

}  // namespace team_ui

void Editor2DScreen::draw_team_panel() { team_ui::draw_panel(team(), &show_team_, "Team###Team2D"); }
void Editor3DScreen::draw_team_panel() { team_ui::draw_panel(team(), &show_team_, "Team###Team3D"); }

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
