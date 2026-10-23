#include "editor/plugins/FrontierPanels.h"
#include "editor/ux/Tooltip.h"

#include "editor/ui/Theme.h"
#include "plugins/survival/SurvivalPlugin.h"
#include "plugins/survival/WorldGen.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

namespace fs = std::filesystem;
using survival::SurvivalPlugin;

ImU32 col(const float* c, float shade = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(std::min(1.0f, c[0] * shade), std::min(1.0f, c[1] * shade),
             std::min(1.0f, c[2] * shade), 1.0f));
}

// --- Saddlebags ---------------------------------------------------------------------

void draw_saddlebags(SurvivalPlugin& p) {
  survival::FrontierSim& sim = p.sim();
  runtime::World* world = p.world();
  if (!sim.active() || !world || sim.mirror()) {
    ImGui::TextColored(theme::TextMuted(), "Saddlebags are packed away.");
    ImGui::TextWrapped("Hit Play to ride the frontier: F mines and swings, G places, C fires, "
                       "Q / X or 1-0 pick from the hotbar.");
    return;
  }
  const survival::GameData& d = sim.data();
  const survival::FrontierSim::Player& pl = sim.player(0);
  ImGui::Text("Grit %d / %d", pl.hp, pl.max_hp);
  ImGui::SameLine();
  ImGui::TextColored(theme::TextMuted(), "| %s, %02d:%02d | %zu critters | %zu settler(s)",
                     sim.night() ? "Night" : "Day",
                     static_cast<int>(sim.phase() * 24.0 + 6.0) % 24,
                     static_cast<int>(sim.phase() * 24.0 * 60.0) % 60, sim.mobs().size(),
                     sim.settlers());
  ImGui::Separator();
  const float cell = 44.0f;
  for (int i = 0; i < survival::Inventory::kSlots; ++i) {
    if (i % survival::Inventory::kHotbar != 0) ImGui::SameLine();
    if (i == survival::Inventory::kHotbar) {
      ImGui::NewLine();
      ImGui::TextColored(theme::TextMuted(), "Bedroll pack");
    }
    const survival::Stack& st = pl.inv.at(i);
    const survival::ItemDef* it = st.empty() ? nullptr : d.item_def(st.item);
    ImGui::PushID(i);
    const bool sel = i == pl.hotbar;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
    char label[32];
    if (it) std::snprintf(label, sizeof(label), "%d", st.count);
    else std::snprintf(label, sizeof(label), " ");
    if (ImGui::Button(label, ImVec2(cell, cell)) && i < survival::Inventory::kHotbar) {
      sim.select(*world, 0, i);
    }
    if (sel) ImGui::PopStyleColor();
    const ImVec2 a = ImGui::GetItemRectMin();
    if (it) {
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 8, a.y + 6), ImVec2(a.x + cell - 8, a.y + 26),
                                                col(it->color), 2.0f);
      if (ImGui::IsItemHovered()) {
        ux::SetTooltip("%s (%s)%s", it->name.c_str(), survival::item_kind_name(it->kind),
                          i < survival::Inventory::kHotbar ? "\nClick to sling it" : "");
      }
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("TS_FRONTIER_SLOT", &i, sizeof(int));
      ImGui::Text("%s", it ? it->name.c_str() : "(empty)");
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("TS_FRONTIER_SLOT")) {
        const int from = *static_cast<const int*>(pay->Data);
        sim.swap(*world, 0, from, i);
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
  }
  ImGui::Separator();
  ImGui::TextColored(theme::Accent(), "Make do");
  ImGui::BeginChild("frontier_recipes", ImVec2(0, 0), false);
  for (int r = 0; r < static_cast<int>(d.recipes.size()); ++r) {
    const survival::RecipeDef& rec = d.recipes[static_cast<std::size_t>(r)];
    const survival::ItemDef* out = d.item_def(rec.makes);
    if (!out) continue;
    std::string why;
    const bool ok = sim.can_craft(0, r, &why);
    ImGui::PushID(r);
    ImGui::BeginDisabled(!ok);
    if (theme::PrimaryButton("Make", ImVec2(52, 0))) sim.craft(*world, 0, r, &why);
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::string needs;
    for (const auto& [item, n] : rec.needs) {
      if (!needs.empty()) needs += ", ";
      needs += std::to_string(n) + " " + d.items[static_cast<std::size_t>(item)].name;
    }
    if (rec.station != survival::kObjNone) {
      needs += std::string(" @ ") + survival::object_def(rec.station).name;
    }
    ImGui::Text("%d %s", rec.count, out->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(ok ? theme::TextMuted() : theme::Warning(), "%s%s%s", needs.c_str(),
                       ok ? "" : "  -- ", ok ? "" : why.c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
}

// --- Prospector ---------------------------------------------------------------------

struct Preview {
  std::uint64_t seed = 0;
  int radius = 0;
  bool valid = false;
  std::vector<survival::Tile> tiles;
  std::array<int, survival::kBiomeCount> biomes{};
  int ores = 0;
  int trees = 0;
  std::uint64_t hash = 0;
};

void build_preview(Preview* pv, std::uint64_t seed, int radius) {
  const survival::WorldGen gen(seed);
  const int n = radius * 2;
  pv->tiles.assign(static_cast<std::size_t>(n * n), survival::Tile{});
  pv->biomes = {};
  pv->ores = pv->trees = 0;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const int tx = x - radius, ty = y - radius;
      const survival::Tile t = gen.tile_at(tx, ty);
      pv->tiles[static_cast<std::size_t>(y * n + x)] = t;
      ++pv->biomes[static_cast<std::size_t>(gen.biome_at(tx, ty))];
      if (t.object >= survival::kCopperOre && t.object <= survival::kGoldOre) ++pv->ores;
      if (t.object == survival::kTree || t.object == survival::kCactus) ++pv->trees;
    }
  }
  std::array<survival::Tile, survival::kChunk * survival::kChunk> chunk{};
  gen.generate(0, 0, chunk.data());
  pv->hash = survival::chunk_hash(chunk.data());
  pv->seed = seed;
  pv->radius = radius;
  pv->valid = true;
}

void draw_prospector(SurvivalPlugin& p) {
  static Preview pv;
  static std::uint64_t seed_edit = 0;
  static bool seeded_edit = false;
  static int radius = 64;
  static std::string status;
  const survival::WorldSettings& ws = p.data().world;
  if (!seeded_edit) {
    seed_edit = ws.seed;
    seeded_edit = true;
  }
  ImGui::TextColored(theme::Accent(), "Claim \"%s\"", ws.world.c_str());
  ImGui::SameLine();
  ImGui::TextColored(theme::TextMuted(), "(survival/world.json, %.0f-minute days)", ws.day_minutes);
  ImGui::SetNextItemWidth(160);
  ImGui::InputScalar("Seed", ImGuiDataType_U64, &seed_edit);
  ImGui::SameLine();
  if (ImGui::Button("Reroll")) {
    seed_edit = (seed_edit * 6364136223846793005ull + 1442695040888963407ull) >> 16;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100);
  ImGui::SliderInt("Reach", &radius, 16, 128, "%d tiles");
  if (!pv.valid || pv.seed != seed_edit || pv.radius != radius) build_preview(&pv, seed_edit, radius);

  const int n = pv.radius * 2;
  const float avail = std::max(120.0f, std::min(ImGui::GetContentRegionAvail().x, 420.0f));
  const float px = avail / static_cast<float>(n);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const survival::Tile& t = pv.tiles[static_cast<std::size_t>(y * n + x)];
      const float* c = t.object != survival::kObjNone ? survival::object_def(t.object).color
                                                     : survival::ground_def(t.ground).color;
      dl->AddRectFilled(ImVec2(o.x + x * px, o.y + y * px),
                        ImVec2(o.x + (x + 1) * px, o.y + (y + 1) * px), col(c));
    }
  }
  const float c0 = static_cast<float>(pv.radius) * px;
  dl->AddRect(ImVec2(o.x + c0 - 4, o.y + c0 - 4), ImVec2(o.x + c0 + 4, o.y + c0 + 4),
              ImGui::GetColorU32(theme::Accent()), 0.0f, 0, 2.0f);
  ImGui::Dummy(ImVec2(avail, avail));
  const float total = static_cast<float>(n * n);
  static const char* names[survival::kBiomeCount] = {"Prairie", "Desert", "Badlands", "Caves"};
  for (int b = 0; b < survival::kBiomeCount; ++b) {
    if (b) ImGui::SameLine();
    ImGui::Text("%s %.0f%%", names[b], 100.0f * static_cast<float>(pv.biomes[static_cast<std::size_t>(b)]) / total);
  }
  ImGui::TextColored(theme::TextMuted(), "%d ore veins, %d trees & cacti, spawn chunk hash %016" PRIx64,
                     pv.ores, pv.trees, pv.hash);
  ImGui::Separator();
  const bool riding = p.sim().active();
  ImGui::BeginDisabled(riding);
  if (theme::PrimaryButton("Burn the claim / regenerate")) ImGui::OpenPopup("Burn the claim?");
  ImGui::EndDisabled();
  if (riding && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ux::SetTooltip("Stop the ride first.");
  }
  if (ImGui::BeginPopupModal("Burn the claim?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Stake seed %" PRIu64 " and burn saves/%s/ (every dug tile, saddlebag and settler)?",
                seed_edit, ws.world.c_str());
    if (theme::PrimaryButton("Burn it")) {
      survival::WorldSettings next = ws;
      next.seed = seed_edit;
      std::string err;
      if (!survival::write_world_settings(p.project_dir(), next, &err)) {
        status = "Couldn't write world.json: " + err;
      } else {
        std::error_code ec;
        const fs::path saves = fs::path(p.project_dir()) / "saves" / ws.world;
        const std::uintmax_t gone = fs::exists(saves, ec) ? fs::remove_all(saves, ec) : 0;
        p.reload_data(&err);
        status = ec ? "Seed staked, but the old save wouldn't burn: " + ec.message()
                    : "Fresh claim staked on seed " + std::to_string(seed_edit) + " (" +
                          std::to_string(gone) + " old file(s) burned).";
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Hold off")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
  for (const std::string& w : p.data_warnings()) ImGui::TextColored(theme::Warning(), "%s", w.c_str());
}

}  // namespace

void install_frontier_panels() {
  SurvivalPlugin::set_editor_hook([](SurvivalPlugin& p, plugin::Registrar& reg) {
    SurvivalPlugin* sp = &p;
    plugin::EditorPanel bags;
    bags.id = "frontier.saddlebags";
    bags.title = "Saddlebags";
    bags.menu_label = "Saddlebags (Frontier)";
    bags.before_begin = [] {
      ImGui::SetNextWindowSize(ImVec2(540, 440), ImGuiCond_FirstUseEver);
    };
    bags.draw = [sp] { draw_saddlebags(*sp); };
    reg.add_editor_panel(std::move(bags));
    plugin::EditorPanel pros;
    pros.id = "frontier.prospector";
    pros.title = "Prospector";
    pros.menu_label = "Prospector (Frontier)";
    pros.before_begin = [] {
      ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);
    };
    pros.draw = [sp] { draw_prospector(*sp); };
    reg.add_editor_panel(std::move(pros));
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
