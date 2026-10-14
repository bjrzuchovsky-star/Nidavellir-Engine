// Play with posse: an in-process GameServer + GameClients (mp::LocalPosse)
// riding the edit scene over loopback, the Network panel (lag sim, riders,
// RTT, bandwidth, snapshot rate, trail map) and the posse Viewport.

#include "editor/screens/Editor2DScreen.h"

#include "editor/launch/GameLauncher.h"
#include "editor/ui/Theme.h"
#include "editor/workspace/SceneIO.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

ImU32 quad_color(const float c[4]) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}

ImTextureID posse_tex_id(std::uint64_t handle) {
  return (ImTextureID)(std::uintptr_t)handle;
}

// One colour per seat, from the Dusk palette.
ImVec4 slot_color(int slot) {
  switch (slot) {
    case 0: return theme::Accent();
    case 1: return theme::Copper();
    case 2: return theme::Success();
    default: return theme::Sand();
  }
}

struct TrailPreset {
  const char* label;
  const char* blurb;
  net::LinkSim sim;
};

constexpr TrailPreset kPresets[] = {
    {"Town", "Same street: no lag", {0, 0, 0.0f}},
    {"Prairie", "60 ms +-15, 2% loss", {60, 15, 0.02f}},
    {"Mountain Pass", "150 ms +-40, 10% loss", {150, 40, 0.10f}},
};

}  // namespace

bool Editor2DScreen::start_posse() {
  if (posse_running()) return true;
  if (is_playing()) stop_play();
  cancel_rename();
  flush_pending_edit();
  if (nudge_pending_save_ || dirty_) {
    nudge_pending_save_ = false;
    mark_dirty_and_autosave();
  }
  mp::PosseConfig cfg;
  cfg.project_dir = project_.path;
  cfg.project_name = project_.name;
  cfg.scene = scene_io::to_doc(workspace_);
  cfg.riders = posse_riders_;
  cfg.sim = posse_sim_;
  cfg.port = 0;
  auto posse = std::make_unique<mp::LocalPosse>();
  std::string err;
  if (!posse->start(std::move(cfg), &err)) {
    note("The posse would not ride: " + err);
    telegraph_.add(runtime::LogLevel::Error, "posse", "Could not ride with the posse: " + err);
    focus_console_ = true;
    return false;
  }
  posse->set_bots(posse_bots_);
  posse_ = std::move(posse);
  posse_view_ = 0;
  show_network_ = true;
  focus_network_ = true;
  play_focus_viewport_ = true;
  pump_posse_logs();
  note("Riding with a posse of " + std::to_string(posse_riders_) + " on UDP " +
       std::to_string(posse_->port()) + ". You're P1: WASD / arrows, E / Space.");
  std::cout << "[Editor2D] posse: server on UDP " << posse_->port() << ", " << posse_riders_
            << " rider(s), lag " << posse_sim_.latency_ms << " ms +-" << posse_sim_.jitter_ms
            << " ms, " << posse_sim_.loss * 100.0f << "% loss\n";
  return true;
}

void Editor2DScreen::stop_posse() {
  if (!posse_) return;
  posse_->stop();
  pump_posse_logs();
  posse_.reset();
  posse_quads_.clear();
  note("Posse broke camp. The edit scene never left.");
  telegraph_.add(runtime::LogLevel::Info, "posse", "Posse broke camp");
}

void Editor2DScreen::toggle_posse() {
  if (posse_running()) {
    stop_posse();
  } else {
    start_posse();
  }
}

void Editor2DScreen::set_posse_riders(int n) {
  posse_riders_ = std::clamp(n, 1, mp::kMaxPosse);
  if (!posse_running()) return;
  std::string err;
  while (static_cast<int>(posse_->clients().size()) < posse_riders_) {
    if (!posse_->add_rider(&err)) {
      note("No room in the saddle: " + err);
      break;
    }
  }
  while (static_cast<int>(posse_->clients().size()) > posse_riders_) {
    posse_->remove_rider();
  }
  posse_view_ = std::min(posse_view_, static_cast<int>(posse_->clients().size()) - 1);
}

void Editor2DScreen::set_posse_sim(const net::LinkSim& sim) {
  posse_sim_ = sim;
  posse_sim_.latency_ms = std::clamp(posse_sim_.latency_ms, 0, 1000);
  posse_sim_.jitter_ms = std::clamp(posse_sim_.jitter_ms, 0, 500);
  posse_sim_.loss = std::clamp(posse_sim_.loss, 0.0f, 0.9f);
  if (posse_running()) posse_->set_link_sim(posse_sim_);
}

void Editor2DScreen::update_posse(double real_seconds, const runtime::PlayerInput& p1) {
  if (!posse_running()) return;
  posse_->set_bots(posse_bots_);
  posse_->update(real_seconds, p1);
  pump_posse_logs();
}

void Editor2DScreen::pump_posse_logs() {
  if (!posse_) return;
  for (const std::string& line : posse_->server().take_log()) {
    telegraph_.add(runtime::LogLevel::Info, "posse", line);
  }
  for (runtime::LogEntry& e : posse_->server().take_world_logs()) {
    telegraph_.add(std::move(e));
  }
  for (const auto& c : posse_->clients()) c->take_events();
}

bool Editor2DScreen::open_posse_client_window() {
  if (!posse_running()) {
    note("Ride with the posse first, then open a client window.");
    return false;
  }
  const std::string exe = launcher::find_client_executable();
  if (exe.empty()) {
    note("ts_client isn't built. Configure with -DTS_BUILD_CLIENT=ON and build it.");
    return false;
  }
  const std::string name = mp::posse_name(4 + posse_windows_++);
  std::string err;
  if (!launcher::spawn_client(exe, "127.0.0.1:" + std::to_string(posse_->port()), name,
                              project_.path, &err)) {
    note("ts_client would not start: " + err);
    return false;
  }
  note(name + " is saddling up in a ts_client window.");
  return true;
}

// --- Viewport while the posse rides -------------------------------------------------

void Editor2DScreen::draw_posse_viewport() {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
  ImVec2 canvas_size = ImGui::GetContentRegionAvail();
  canvas_size.x = std::max(canvas_size.x, 64.0f);
  canvas_size.y = std::max(canvas_size.y, 64.0f);
  const ImVec2 canvas_end(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);
  ImGui::InvisibleButton("##PosseCanvas", canvas_size);
  draw->AddRectFilled(canvas_pos, canvas_end, theme::U32(theme::CanvasBg()));

  const auto& clients = posse_->clients();
  if (clients.empty()) return;
  posse_view_ = std::clamp(posse_view_, 0, static_cast<int>(clients.size()) - 1);
  mp::GameClient& c = *clients[static_cast<std::size_t>(posse_view_)];
  runtime::World& world = c.world();
  if (!c.has_snapshot()) {
    const char* msg = "Riding in...";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    draw->AddText(ImVec2(canvas_pos.x + (canvas_size.x - ts.x) * 0.5f,
                         canvas_pos.y + (canvas_size.y - ts.y) * 0.5f),
                  theme::U32(theme::TextMuted()), msg);
    return;
  }
  runtime::CameraView cam;
  for (const runtime::Actor& a : world.actors()) {
    if (a.data.camera) {
      cam.zoom = std::max(0.05f, a.data.camera->zoom);
      break;
    }
  }
  if (const runtime::Actor* me = world.find(c.rider())) {
    cam.x = me->data.center_x();
    cam.y = me->data.center_y();
  }
  auto to_screen = [&](float wx, float wy) {
    return ImVec2(canvas_pos.x + canvas_size.x * 0.5f + (wx - cam.x) * cam.zoom,
                  canvas_pos.y + canvas_size.y * 0.5f + (wy - cam.y) * cam.zoom);
  };
  world.build_draw_list(
      runtime::World::view_rect(cam, canvas_size.x, canvas_size.y), 1.0f,
      [this](const std::string& rel, int* w, int* h) {
        const TextureInfo& t = texture(rel);
        if (!t.ok) return false;
        *w = t.width;
        *h = t.height;
        return true;
      },
      &posse_quads_);
  draw->PushClipRect(canvas_pos, canvas_end, true);
  for (const runtime::DrawQuad& q : posse_quads_) {
    const ImVec2 p0 = to_screen(q.x0, q.y0);
    const ImVec2 p1 = to_screen(q.x1, q.y1);
    const std::uint64_t handle = q.image ? texture(*q.image).handle : 0;
    if (handle != 0) {
      draw->AddImage(posse_tex_id(handle), p0, p1, ImVec2(q.u0, q.v0), ImVec2(q.u1, q.v1),
                     quad_color(q.rgba));
    } else {
      draw->AddRectFilled(p0, p1, quad_color(q.rgba));
    }
  }
  // Rider tags, plus a ghost box where the server has them right now.
  const runtime::World& sw = posse_->server().world();
  for (const runtime::Actor& a : world.actors()) {
    if (!a.data.player || a.hidden) continue;
    const int slot = a.data.player->slot;
    if (const runtime::Actor* s = sw.find(a.data.id)) {
      draw->AddRect(to_screen(s->data.x, s->data.y),
                    to_screen(s->data.x + s->data.w, s->data.y + s->data.h),
                    theme::U32(slot_color(slot), 0.55f), 0.0f, 0, 1.0f);
    }
    char tag[48];
    std::snprintf(tag, sizeof(tag), "P%d%s", slot + 1, a.data.id == c.rider() ? " (you)" : "");
    const ImVec2 at = to_screen(a.data.center_x(), a.data.y);
    const float tw = ImGui::CalcTextSize(tag).x;
    draw->AddText(ImVec2(at.x - tw * 0.5f, at.y - ImGui::GetTextLineHeight() - 2.0f),
                  theme::U32(slot_color(slot)), tag);
  }
  // Toasts on this rider's HUD.
  float y = canvas_end.y - 12.0f;
  const auto toasts = c.toasts();
  for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
    const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str());
    y -= ts.y + 12.0f;
    const ImVec2 p0(canvas_pos.x + (canvas_size.x - ts.x) * 0.5f - 10.0f, y);
    const ImVec2 p1(p0.x + ts.x + 20.0f, y + ts.y + 8.0f);
    draw->AddRectFilled(p0, p1, theme::U32(theme::PanelBg(), 0.92f), theme::metrics::kRadiusCard);
    draw->AddRect(p0, p1, theme::U32(theme::Copper()), theme::metrics::kRadiusCard);
    draw->AddText(ImVec2(p0.x + 10.0f, p0.y + 4.0f), theme::U32(theme::Text()), it->text.c_str());
  }
  // Banner: whose eyes, and how rough the trail is.
  char banner[192];
  std::snprintf(banner, sizeof(banner), "POSSE  |  P%d %s's eyes  |  RTT %.0f ms  |  %d ms +-%d, %.0f%% loss",
                c.slot() + 1, c.name().c_str(), c.stats().rtt_ms, posse_sim_.latency_ms,
                posse_sim_.jitter_ms, static_cast<double>(posse_sim_.loss) * 100.0);
  draw->AddRectFilled(canvas_pos, ImVec2(canvas_end.x, canvas_pos.y + 24.0f),
                      theme::U32(theme::Charcoal(), 0.75f));
  draw->AddText(ImVec2(canvas_pos.x + 10.0f, canvas_pos.y + 4.0f), theme::U32(theme::Accent()),
                banner);
  draw->PopClipRect();
}

// --- Network panel -------------------------------------------------------------------

void Editor2DScreen::draw_posse_map(float height) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
  const ImVec2 p1(p0.x + w, p0.y + height);
  ImGui::Dummy(ImVec2(w, height));
  draw->AddRectFilled(p0, p1, theme::U32(theme::CanvasBg()), theme::metrics::kRadiusCard);
  draw->AddRect(p0, p1, theme::U32(theme::Border()), theme::metrics::kRadiusCard);
  if (!posse_running()) {
    const char* msg = "No posse riding. Ride with Posse to see the trail.";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    draw->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + (height - ts.y) * 0.5f),
                  theme::U32(theme::TextMuted()), msg);
    return;
  }
  const runtime::World& sw = posse_->server().world();
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const runtime::Actor& a : sw.actors()) {
    if (a.data.tilemap || a.data.camera) continue;
    x0 = std::min(x0, a.data.x);
    y0 = std::min(y0, a.data.y);
    x1 = std::max(x1, a.data.x + a.data.w);
    y1 = std::max(y1, a.data.y + a.data.h);
  }
  if (x1 <= x0 || y1 <= y0) return;
  const float pad = 12.0f;
  const float s = std::min((w - 2 * pad) / (x1 - x0), (height - 2 * pad) / (y1 - y0));
  auto map = [&](float x, float y) {
    return ImVec2(p0.x + pad + (x - x0) * s, p0.y + pad + (y - y0) * s);
  };
  draw->PushClipRect(p0, p1, true);
  for (const runtime::Actor& a : sw.actors()) {
    if (a.data.player || a.data.tilemap || a.data.camera || a.hidden) continue;
    draw->AddRectFilled(map(a.data.x, a.data.y), map(a.data.x + a.data.w, a.data.y + a.data.h),
                        theme::U32(theme::Sand(), a.data.collider ? 0.35f : 0.15f));
  }
  const auto& clients = posse_->clients();
  const mp::GameClient* eyes =
      clients.empty() ? nullptr
                      : clients[static_cast<std::size_t>(
                                    std::clamp(posse_view_, 0, static_cast<int>(clients.size()) - 1))]
                            .get();
  for (const mp::ServerRider& r : posse_->server().riders()) {
    const runtime::Actor* a = sw.find(r.rider);
    if (!a) continue;
    const ImVec4 col = slot_color(r.slot);
    const ImVec2 c = map(a->data.center_x(), a->data.center_y());
    draw->AddCircleFilled(c, 5.0f, theme::U32(col));
    float vx = 0, vy = 0;
    if (eyes && eyes->drawn(r.rider, &vx, &vy)) {
      const ImVec2 v = map(vx + a->data.w * 0.5f, vy + a->data.h * 0.5f);
      draw->AddCircle(v, 8.0f, theme::U32(col, 0.85f), 16, 1.5f);
      draw->AddLine(c, v, theme::U32(col, 0.5f));
    }
    char tag[8];
    std::snprintf(tag, sizeof(tag), "P%d", r.slot + 1);
    draw->AddText(ImVec2(c.x + 9.0f, c.y - 7.0f), theme::U32(col), tag);
  }
  draw->PopClipRect();
}

void Editor2DScreen::draw_network_panel() {
  const bool running = posse_running();
  theme::SectionHeader("Posse", "server + riders in this editor");
  if (!running) {
    int riders = posse_riders_;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderInt("Riders##Posse", &riders, 1, mp::kMaxPosse)) set_posse_riders(riders);
    ImGui::SameLine();
    ImGui::Checkbox("Bot reins for P2-P4", &posse_bots_);
    if (theme::PrimaryButton("Ride with Posse", ImVec2(150, 0))) start_posse();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Start a local server on a loopback port and %d client(s) in this "
                        "editor, riding a copy of the scene", posse_riders_);
    }
    ImGui::TextDisabled("You ride P1 (WASD / arrows, E / Space). Edits wait until you're back.");
  } else {
    const mp::GameServer& sv = posse_->server();
    ImGui::TextColored(theme::Success(), "Riding");
    ImGui::SameLine();
    ImGui::Text("| server UDP %u | tick %llu | %d of %d seats", static_cast<unsigned>(sv.port()),
                static_cast<unsigned long long>(sv.tick()), sv.rider_count(), mp::kMaxPosse);
    if (theme::DangerButton("Break Camp", ImVec2(110, 0))) {
      stop_posse();
      return;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(static_cast<int>(posse_->clients().size()) >= mp::kMaxPosse ||
                         sv.rider_count() >= mp::kMaxPosse);
    if (theme::SecondaryButton("Saddle Another")) {
      set_posse_riders(static_cast<int>(posse_->clients().size()) + 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(posse_->clients().size() <= 1);
    if (theme::SecondaryButton("Send One Home")) {
      set_posse_riders(static_cast<int>(posse_->clients().size()) - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sv.rider_count() >= mp::kMaxPosse);
    if (theme::CopperButton("Open Client Window")) open_posse_client_window();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Start ts_client in its own window, riding into this server");
    }
    ImGui::Checkbox("Bot reins for P2-P4", &posse_bots_);
  }

  theme::SectionHeader("Rough Trail", "lag sim on every rider's line");
  net::LinkSim sim = posse_sim_;
  float loss_pct = sim.loss * 100.0f;
  bool changed = false;
  ImGui::SetNextItemWidth(180.0f);
  changed |= ImGui::SliderInt("Latency (ms each way)", &sim.latency_ms, 0, 400);
  ImGui::SetNextItemWidth(180.0f);
  changed |= ImGui::SliderInt("Jitter (+- ms)", &sim.jitter_ms, 0, 150);
  ImGui::SetNextItemWidth(180.0f);
  if (ImGui::SliderFloat("Loss (%)", &loss_pct, 0.0f, 50.0f, "%.0f%%")) {
    sim.loss = loss_pct / 100.0f;
    changed = true;
  }
  for (const TrailPreset& p : kPresets) {
    if (theme::SecondaryButton(p.label)) {
      sim = p.sim;
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.blurb);
    ImGui::SameLine();
  }
  ImGui::NewLine();
  if (changed) set_posse_sim(sim);

  if (!running) {
    theme::SectionHeader("Trail Map");
    draw_posse_map(120.0f);
    return;
  }
  const mp::GameServer& sv = posse_->server();
  const mp::ServerStats& st = sv.stats();
  theme::SectionHeader("Server", "authoritative, 60 Hz");
  ImGui::Text("Snapshots %.0f/s per rider | %zu bytes each | out %.1f KB/s | in %.1f KB/s",
              st.snapshot_hz, st.last_snapshot_bytes, st.kbps_out, st.kbps_in);
  ImGui::TextDisabled("Rode in %llu | rode out %llu | turned away %llu",
                      static_cast<unsigned long long>(st.joins),
                      static_cast<unsigned long long>(st.leaves),
                      static_cast<unsigned long long>(st.rejected));

  theme::SectionHeader("Riders", "who's in the saddle");
  const auto& clients = posse_->clients();
  if (ImGui::BeginTable("##PosseRiders", 8,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Seat");
    ImGui::TableSetupColumn("Rider");
    ImGui::TableSetupColumn("RTT");
    ImGui::TableSetupColumn("Snaps/s");
    ImGui::TableSetupColumn("In KB/s");
    ImGui::TableSetupColumn("Out KB/s");
    ImGui::TableSetupColumn("Fixes");
    ImGui::TableSetupColumn("Eyes");
    ImGui::TableHeadersRow();
    for (const mp::ServerRider& r : sv.riders()) {
      int index = -1;
      for (std::size_t i = 0; i < clients.size(); ++i) {
        if (clients[i]->joined() && clients[i]->slot() == r.slot) index = static_cast<int>(i);
      }
      const mp::GameClient* c = index >= 0 ? clients[static_cast<std::size_t>(index)].get() : nullptr;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(slot_color(r.slot), "P%d", r.slot + 1);
      ImGui::TableNextColumn();
      ImGui::Text("%s%s", r.name.c_str(), c ? "" : " (window)");
      ImGui::TableNextColumn();
      ImGui::Text("%.0f ms", c ? c->stats().rtt_ms : static_cast<double>(r.rtt_ms));
      ImGui::TableNextColumn();
      if (c) {
        ImGui::Text("%.0f", c->stats().snapshot_hz);
      } else {
        ImGui::TextDisabled("%.0f", st.snapshot_hz);
      }
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", c ? c->stats().kbps_in : r.kbps_out);
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", c ? c->stats().kbps_out : r.kbps_in);
      ImGui::TableNextColumn();
      if (c) {
        ImGui::Text("%llu", static_cast<unsigned long long>(c->stats().corrections));
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Reconciliations that moved the predicted rider\nlast %.2f px, worst "
                            "%.2f px\n%zu inputs in flight",
                            static_cast<double>(c->stats().last_correction_px),
                            static_cast<double>(c->stats().max_correction_px),
                            c->stats().pending_inputs);
        }
      } else {
        ImGui::TextDisabled("-");
      }
      ImGui::TableNextColumn();
      if (c) {
        ImGui::PushID(index);
        bool mine = posse_view_ == index;
        if (theme::ToggleButton("Look", &mine, ImVec2(44, 0)) && mine) posse_view_ = index;
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }
  theme::SectionHeader("Trail Map", "dot = server, ring = chosen rider's view");
  draw_posse_map(std::max(140.0f, ImGui::GetContentRegionAvail().y - 8.0f));
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
