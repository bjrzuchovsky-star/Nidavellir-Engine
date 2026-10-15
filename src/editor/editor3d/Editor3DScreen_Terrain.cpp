// Terrain + foliage editing: create terrains, sculpt / paint / foliage brush
// strokes in the viewport (one undo step per stroke, with a heightfield
// snapshot), and the "Terrain + Foliage" panel.
#include "editor/editor3d/Editor3DScreen.h"

#include "editor/ui/Theme.h"
#include "terrain/Foliage.h"
#include "terrain/TerrainView.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Vec3;

namespace {

constexpr int kResolutions[] = {65, 129, 257, 513, 1025};
constexpr const char* kLooks[] = {"meadow", "desert", "snowy"};

void apply_look(terrain::Terrain3D& c, const std::string& look) {
  auto preset = [&](const char* name) {
    terrain::FoliageType t;
    if (terrain::foliage_preset(name, &t)) c.foliage.push_back(t);
  };
  c.foliage.clear();
  if (look == "desert") {
    c.layers[0] = {"builtin:sand", {1, 1, 1}, 4.0f};
    c.layers[1] = {"builtin:dirt", {1.05f, 0.9f, 0.75f}, 3.0f};
    c.layers[2] = {"builtin:rock", {1.0f, 0.85f, 0.7f}, 6.0f};
    c.layers[3] = {"builtin:grass", {0.9f, 0.85f, 0.55f}, 4.0f};
    preset("cactus");
    preset("bush");
    preset("rock");
  } else if (look == "snowy") {
    c.layers[0] = {"builtin:snow", {1, 1, 1}, 4.0f};
    c.layers[1] = {"builtin:rock", {1, 1, 1}, 6.0f};
    c.layers[2] = {"builtin:dirt", {1, 1, 1}, 3.0f};
    c.layers[3] = {"builtin:grass", {0.8f, 0.9f, 0.8f}, 4.0f};
    preset("pine");
    preset("rock");
  } else {
    preset("grass");
    preset("pine");
    preset("rock");
  }
}

}  // namespace

std::uint64_t Editor3DScreen::terrain_target() const {
  if (const scene3d::Entity3D* e = find(primary()); e && e->terrain) return e->id;
  for (const scene3d::Entity3D& e : scene_.entities)
    if (e.terrain && e.visible) return e.id;
  return 0;
}

bool Editor3DScreen::terrain_ray(const render3d::Ray& ray, float* t, std::uint64_t* id) const {
  float best = 1e30f;
  for (const scene3d::Entity3D& e : scene_.entities) {
    if (!e.terrain || !e.visible || effectively_hidden(e)) continue;
    std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, *e.terrain);
    float tt = 0.0f;
    if (d && terrain::raycast(*d, ray.origin - e.transform.position, ray.dir, 2000.0f, &tt) &&
        tt > 0.0f && tt < best) {
      best = tt;
      *id = e.id;
    }
  }
  if (best >= 1e29f) return false;
  *t = best;
  return true;
}

std::uint64_t Editor3DScreen::create_terrain(int resolution, float size, bool hills,
                                             const std::string& look) {
  if (playing()) return 0;
  if (!terrain::valid_resolution(resolution)) resolution = 129;
  size = std::clamp(size, 8.0f, 8192.0f);
  // Plants sway: give a calm scene a breeze (own undo step).
  if (scene_.environment.wind_strength <= 0.0f) {
    render3d::Environment env = scene_.environment;
    env.wind_strength = 0.3f;
    env.wind_gust = 0.25f;
    set_environment(env, "Wind for foliage");
  }
  scene3d::Entity3D e;
  e.name = "Terrain";
  terrain::Terrain3D c;
  c.resolution = resolution;
  c.size = size;
  apply_look(c, look);
  // A data file name that no other terrain uses.
  for (int n = 1;; ++n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "terrain/terrain_%d.tsterrain", n);
    bool used = false;
    for (const scene3d::Entity3D& o : scene_.entities)
      if (o.terrain && o.terrain->data == buf) used = true;
    std::error_code ec;
    if (!used && !std::filesystem::exists(std::filesystem::path(project_.path) / buf, ec)) {
      c.data = buf;
      break;
    }
  }
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, c);
  terrain::reset(*d, resolution, size);
  if (hills) {
    terrain::generate_hills(*d, static_cast<std::uint32_t>(scene_.next_id() * 7919u + 17u),
                            std::max(2.0f, size * 0.06f), std::max(8.0f, size * 0.25f));
  }
  e.terrain = c;
  e.transform.position = {0, 0, 0};
  const std::uint64_t id = add_entity(e, "Create terrain");
  if (id) {
    select(id);
    terrain_tool_.foliage_type = 0;
  }
  return id;
}

// ---- Undo -----------------------------------------------------------------------

std::vector<Editor3DScreen::TerrainSnap> Editor3DScreen::swap_terrains(std::vector<TerrainSnap> snaps) {
  for (TerrainSnap& s : snaps) {
    std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, s.comp);
    if (!d) continue;
    std::swap(d->heights, s.data.heights);
    std::swap(d->splat, s.data.splat);
    std::swap(d->foliage, s.data.foliage);
    std::swap(d->res, s.data.res);
    std::swap(d->size, s.data.size);
    // Caches can't replay an edit log across a swap: one full edit.
    d->edits.clear();
    terrain::note_edit(*d, terrain::full_rect(*d), true, true, true);
  }
  return snaps;
}

void Editor3DScreen::push_terrain_undo(const std::string& label, const scene3d::Scene3D& before,
                                       std::vector<TerrainSnap> data) {
  undo_.push_back({label, before, std::move(data)});
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  redo_.clear();
  autosave();
}

// ---- Strokes ----------------------------------------------------------------------

bool Editor3DScreen::terrain_stroke_begin(std::uint64_t id) {
  if (playing() || stroke_) return false;
  const scene3d::Entity3D* e = find(id);
  if (!e || !e->terrain || effectively_locked(*e)) return false;
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, *e->terrain);
  if (!d || !d->valid()) return false;
  Stroke s;
  s.id = id;
  s.comp = *e->terrain;
  s.before = *d;
  s.before.edits.clear();
  s.seed = static_cast<std::uint32_t>(undo_.size() * 2654435761u + d->revision + 1);
  stroke_ = std::move(s);
  return true;
}

bool Editor3DScreen::terrain_stroke(const Vec3& world, float dt, bool invert) {
  if (!stroke_) return false;
  const scene3d::Entity3D* e = find(stroke_->id);
  if (!e || !e->terrain) return false;
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, *e->terrain);
  if (!d || !d->valid()) return false;
  const float x = world.x - e->transform.position.x;
  const float z = world.z - e->transform.position.z;
  dt = std::clamp(dt, 0.0f, 0.1f);
  const TerrainTool& tt = terrain_tool_;
  if (tt.mode == 2) {
    terrain::FoliageBrush b;
    b.radius = tt.radius;
    b.seed = stroke_->seed;
    const bool erase = tt.erase != invert;
    if (erase) {
      b.type = tt.foliage_type < 0 ? -1 : tt.foliage_type;
      b.fill = std::clamp(tt.fill * dt * 6.0f, 0.0f, 1.0f);
      return terrain::erase_foliage(*d, b, x, z) > 0;
    }
    if (tt.foliage_type < 0 || tt.foliage_type >= static_cast<int>(e->terrain->foliage.size())) return false;
    b.type = tt.foliage_type;
    b.fill = tt.fill;
    return terrain::paint_foliage(*d, e->terrain->foliage, b, x, z) > 0;
  }
  terrain::Brush b;
  b.radius = tt.radius;
  b.strength = tt.strength;
  b.hardness = tt.hardness;
  b.noise_scale = tt.noise_scale;
  b.seed = stroke_->seed;
  if (tt.mode == 1) {
    b.op = terrain::BrushOp::Paint;
    b.layer = std::clamp(tt.layer, 0, terrain::kLayers - 1);
    if (invert) b.layer = 0;  // Ctrl: back to the base layer
  } else {
    b.op = tt.op;
    if (invert && b.op == terrain::BrushOp::Raise) b.op = terrain::BrushOp::Lower;
    else if (invert && b.op == terrain::BrushOp::Lower) b.op = terrain::BrushOp::Raise;
    if (b.op == terrain::BrushOp::Flatten) {
      // Flatten to the height under the first stroke point.
      if (!stroke_->has_target) {
        stroke_->flatten_target = terrain::height_at(*d, x, z);
        stroke_->has_target = true;
      }
      b.target = stroke_->flatten_target;
    }
  }
  const terrain::Rect r = terrain::apply_brush(*d, b, x, z, dt);
  return !r.empty();
}

void Editor3DScreen::terrain_stroke_end() {
  if (!stroke_) return;
  Stroke s = std::move(*stroke_);
  stroke_.reset();
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, s.comp);
  if (!d) return;
  if (d->heights == s.before.heights && d->splat == s.before.splat && d->foliage == s.before.foliage)
    return;
  static const char* kNames[] = {"Sculpt terrain", "Paint terrain", "Paint foliage"};
  std::vector<TerrainSnap> snaps;
  snaps.push_back({s.comp, std::move(s.before)});
  push_terrain_undo(kNames[std::clamp(terrain_tool_.mode, 0, 2)], scene_, std::move(snaps));
}

bool Editor3DScreen::save_terrains(std::string* error) {
  for (const scene3d::Entity3D& e : scene_.entities) {
    if (!e.terrain || e.terrain->data.empty()) continue;
    if (!terrain::save(project_.path, *e.terrain, error)) return false;
  }
  return true;
}

// ---- Viewport ---------------------------------------------------------------------

bool Editor3DScreen::terrain_viewport(float mx, float my, float vw, float vh, float ox, float oy) {
  brush_hit_ = false;
  if (!terrain_tool_.active || playing()) {
    if (stroke_) terrain_stroke_end();
    return false;
  }
  const std::uint64_t target = terrain_target();
  const scene3d::Entity3D* e = find(target);
  if (!e || !e->terrain) {
    if (stroke_) terrain_stroke_end();
    return false;
  }
  ImGuiIO& io = ImGui::GetIO();
  const render3d::Camera3D cam = editor_camera();
  const render3d::Ray ray = cam.screen_ray(mx, my, vw, vh);
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, *e->terrain);
  float t = 0.0f;
  if (d && viewport_hovered_ &&
      terrain::raycast(*d, ray.origin - e->transform.position, ray.dir, 2000.0f, &t)) {
    brush_hit_ = true;
    brush_at_ = ray.origin + ray.dir * t;
  }
  // [ and ] resize the brush.
  if (viewport_hovered_) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) terrain_tool_.radius = std::max(0.5f, terrain_tool_.radius / 1.2f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) terrain_tool_.radius = std::min(200.0f, terrain_tool_.radius * 1.2f);
  }
  if (io.KeyAlt) {  // Alt = camera orbit as usual
    if (stroke_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) terrain_stroke_end();
    return stroke_.has_value();
  }
  if (brush_hit_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) terrain_stroke_begin(target);
  if (stroke_) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (brush_hit_) {
        // Shift: smooth whatever the mode.
        if (io.KeyShift && terrain_tool_.mode == 0) {
          const terrain::BrushOp keep = terrain_tool_.op;
          terrain_tool_.op = terrain::BrushOp::Smooth;
          terrain_stroke(brush_at_, io.DeltaTime, false);
          terrain_tool_.op = keep;
        } else {
          terrain_stroke(brush_at_, io.DeltaTime, io.KeyCtrl);
        }
      }
    } else {
      terrain_stroke_end();
    }
  }
  // Brush ring draped over the ground.
  if (brush_hit_ && d) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = terrain_tool_.mode == 2 ? IM_COL32(120, 230, 120, 230)
                      : terrain_tool_.mode == 1 ? IM_COL32(120, 190, 255, 230)
                                                : IM_COL32(255, 210, 90, 230);
    const Vec3 o = e->transform.position;
    for (int ring = 0; ring < 2; ++ring) {
      const float r = terrain_tool_.radius * (ring == 0 ? 1.0f : std::max(0.05f, terrain_tool_.hardness));
      ImVec2 prev{};
      bool have = false;
      for (int i = 0; i <= 48; ++i) {
        const float a = static_cast<float>(i) / 48.0f * 6.2831853f;
        const float wx = brush_at_.x + std::cos(a) * r, wz = brush_at_.z + std::sin(a) * r;
        const float wy = terrain::height_at(*d, wx - o.x, wz - o.z) + o.y + 0.05f;
        float sx = 0, sy = 0;
        if (!cam.project({wx, wy, wz}, vw, vh, &sx, &sy)) {
          have = false;
          continue;
        }
        const ImVec2 p{ox + sx, oy + sy};
        if (have) dl->AddLine(prev, p, col, ring == 0 ? 2.0f : 1.0f);
        prev = p;
        have = true;
      }
    }
  }
  return true;  // the left button belongs to the brush while it's on
}

// ---- Panel ----------------------------------------------------------------------

void Editor3DScreen::draw_terrain_panel() {
  static int new_res = 1;
  static float new_size = 128.0f;
  static bool new_hills = true;
  static int new_look = 0;
  auto create_ui = [&] {
    ImGui::Combo("Resolution", &new_res, "65\0" "129\0" "257\0" "513\0" "1025\0");
    ImGui::DragFloat("Size (m)", &new_size, 1.0f, 8.0f, 8192.0f, "%.0f");
    ImGui::Combo("Look", &new_look, "meadow\0desert\0snowy\0");
    ImGui::Checkbox("Hills", &new_hills);
    if (ImGui::Button("Create terrain"))
      create_terrain(kResolutions[new_res], new_size, new_hills, kLooks[new_look]);
  };
  const std::uint64_t id = terrain_target();
  const scene3d::Entity3D* cur = find(id);
  if (!cur || !cur->terrain) {
    ImGui::TextWrapped("No terrain in this scene yet.");
    create_ui();
    return;
  }
  scene3d::Entity3D e = *cur;
  terrain::Terrain3D& c = *e.terrain;
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, c);
  bool changed = false;            // live edit of the component
  const char* commit = nullptr;    // push one undo step with this label
  auto track = [&](const char* label) {
    if (ImGui::IsItemActivated() && !terrain_edit_before_) terrain_edit_before_ = scene_;
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = label;
  };
  auto now = [&](const char* label) {  // discrete widgets: one step right away
    if (!terrain_edit_before_) terrain_edit_before_ = scene_;
    commit = label;
    changed = true;
  };

  ImGui::Text("%s", e.name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", c.data.empty() ? "unsaved" : c.data.c_str());
  ImGui::Checkbox("Brushes on", &terrain_tool_.active);
  ImGui::SameLine();
  ImGui::TextDisabled("LMB paint, Ctrl invert, Shift smooth, [ ] size");

  ImGui::Spacing();
  if (ImGui::BeginTabBar("##terrain_modes")) {
    if (ImGui::BeginTabItem("Sculpt")) {
      terrain_tool_.mode = 0;
      static const terrain::BrushOp kOps[] = {terrain::BrushOp::Raise, terrain::BrushOp::Lower,
                                              terrain::BrushOp::Smooth, terrain::BrushOp::Flatten,
                                              terrain::BrushOp::Noise};
      for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(terrain::brush_op_name(kOps[i]), terrain_tool_.op == kOps[i]))
          terrain_tool_.op = kOps[i];
      }
      if (terrain_tool_.op == terrain::BrushOp::Noise)
        ImGui::SliderFloat("Noise size (m)", &terrain_tool_.noise_scale, 0.5f, 32.0f);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Paint")) {
      terrain_tool_.mode = 1;
      for (int i = 0; i < terrain::kLayers; ++i) {
        char label[160];
        std::snprintf(label, sizeof(label), "%d: %s##layer%d", i + 1, c.layers[i].texture.c_str(), i);
        if (ImGui::RadioButton(label, terrain_tool_.layer == i)) terrain_tool_.layer = i;
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Foliage")) {
      terrain_tool_.mode = 2;
      ImGui::Checkbox("Erase", &terrain_tool_.erase);
      ImGui::SameLine();
      ImGui::SliderFloat("Fill", &terrain_tool_.fill, 0.02f, 1.0f);
      if (terrain_tool_.erase && ImGui::RadioButton("Every type", terrain_tool_.foliage_type < 0))
        terrain_tool_.foliage_type = -1;
      for (int i = 0; i < static_cast<int>(c.foliage.size()); ++i) {
        char label[160];
        std::snprintf(label, sizeof(label), "%s (%d)##ft%d", c.foliage[i].name.c_str(),
                      d ? terrain::count_foliage(*d, i) : 0, i);
        if (ImGui::RadioButton(label, terrain_tool_.foliage_type == i)) terrain_tool_.foliage_type = i;
      }
      if (c.foliage.empty()) ImGui::TextDisabled("Add a foliage type below.");
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::SliderFloat("Radius", &terrain_tool_.radius, 0.5f, 100.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
  if (terrain_tool_.mode != 2) {
    ImGui::SliderFloat("Strength", &terrain_tool_.strength, 0.1f, 40.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Hardness", &terrain_tool_.hardness, 0.0f, 1.0f);
  }

  theme::SectionHeader("Layers");
  for (int i = 0; i < terrain::kLayers; ++i) {
    ImGui::PushID(i);
    terrain::TerrainLayer& L = c.layers[i];
    char head[32];
    std::snprintf(head, sizeof(head), "Layer %d", i + 1);
    if (ImGui::TreeNode(head)) {
      if (ImGui::BeginCombo("Texture", L.texture.c_str())) {
        for (const std::string& n : terrain::builtin_layer_names()) {
          const std::string full = "builtin:" + n;
          if (ImGui::Selectable(full.c_str(), L.texture == full)) {
            L.texture = full;
            now("Terrain layer");
          }
        }
        for (const AssetEntry& a : assets_) {
          if (!a.image) continue;
          if (ImGui::Selectable(a.rel.c_str(), L.texture == a.rel)) {
            L.texture = a.rel;
            now("Terrain layer");
          }
        }
        ImGui::EndCombo();
      }
      changed |= ImGui::ColorEdit3("Tint", L.tint);
      track("Layer tint");
      changed |= ImGui::DragFloat("Tiling (m)", &L.tiling, 0.05f, 0.25f, 256.0f);
      track("Layer tiling");
      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  theme::SectionHeader("Foliage types");
  int remove_type = -1;
  for (int i = 0; i < static_cast<int>(c.foliage.size()); ++i) {
    ImGui::PushID(1000 + i);
    terrain::FoliageType& f = c.foliage[i];
    const bool open = ImGui::TreeNode("##ft", "%s", f.name.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 20);
    if (ImGui::SmallButton("x")) remove_type = i;
    if (open) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", f.name.c_str());
      if (ImGui::InputText("Name", buf, sizeof(buf))) {
        f.name = buf;
        changed = true;
      }
      track("Foliage name");
      if (ImGui::BeginCombo("Mesh", f.mesh.c_str())) {
        for (const std::string& n : terrain::foliage_mesh_names())
          if (ImGui::Selectable(n.c_str(), f.mesh == n)) {
            f.mesh = n;
            now("Foliage mesh");
          }
        for (const AssetEntry& a : assets_) {
          if (!a.model || a.rel.rfind("primitive:", 0) == 0) continue;
          if (ImGui::Selectable(a.rel.c_str(), f.mesh == a.rel)) {
            f.mesh = a.rel;
            now("Foliage mesh");
          }
        }
        ImGui::EndCombo();
      }
      changed |= ImGui::DragFloat("Density (/m2)", &f.density, 0.01f, 0.001f, 64.0f, "%.3f");
      track("Foliage density");
      changed |= ImGui::DragFloatRange2("Scale", &f.scale_min, &f.scale_max, 0.01f, 0.05f, 20.0f);
      track("Foliage scale");
      changed |= ImGui::SliderFloat("Wind sway", &f.sway, 0.0f, 3.0f);
      track("Foliage sway");
      changed |= ImGui::DragFloatRange2("Fade (m)", &f.fade_start, &f.fade_end, 0.5f, 1.0f, 2000.0f);
      track("Foliage fade");
      changed |= ImGui::ColorEdit3("Colour A", f.color_a);
      track("Foliage colour");
      changed |= ImGui::ColorEdit3("Colour B", f.color_b);
      track("Foliage colour");
      changed |= ImGui::SliderFloat("Max slope", &f.slope_max, 0.0f, 90.0f, "%.0f deg");
      track("Foliage slope");
      changed |= ImGui::SliderInt("Only on layer", &f.layer, -1, terrain::kLayers - 1,
                                  f.layer < 0 ? "anywhere" : "%d");
      track("Foliage layer");
      if (ImGui::Checkbox("Casts shadows", &f.cast_shadows)) now("Foliage shadows");
      if (ImGui::Checkbox("Collides", &f.collide)) now("Foliage collider");
      if (f.collide) {
        changed |= ImGui::DragFloat("Collider radius", &f.collide_radius, 0.01f, 0.05f, 10.0f);
        track("Foliage collider");
        changed |= ImGui::DragFloat("Collider height", &f.collide_height, 0.05f, 0.1f, 60.0f);
        track("Foliage collider");
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (ImGui::BeginCombo("##addfoliage", "+ Add foliage type")) {
    for (const std::string& n : terrain::foliage_preset_names()) {
      if (ImGui::Selectable(n.c_str())) {
        terrain::FoliageType f;
        terrain::foliage_preset(n, &f);
        c.foliage.push_back(f);
        terrain_tool_.foliage_type = static_cast<int>(c.foliage.size()) - 1;
        now("Add foliage type");
      }
    }
    ImGui::EndCombo();
  }

  theme::SectionHeader("Terrain");
  changed |= ImGui::DragFloat("Size (m)##t", &c.size, 0.5f, 8.0f, 8192.0f, "%.0f");
  track("Terrain size");
  ImGui::TextDisabled("Resolution %d x %d (cell %.2f m)", d ? d->res : c.resolution, d ? d->res : c.resolution,
                      d && d->valid() ? d->cell() : 0.0f);
  int chunk_ix = c.chunk <= 16 ? 0 : c.chunk <= 32 ? 1 : 2;
  if (ImGui::Combo("Chunk (quads)", &chunk_ix, "16\0" "32\0" "64\0")) {
    c.chunk = 16 << chunk_ix;
    now("Terrain chunks");
  }
  changed |= ImGui::DragFloat("LOD distance", &c.lod_distance, 0.5f, 5.0f, 2000.0f, "%.0f m");
  track("Terrain LOD");
  changed |= ImGui::DragFloat("Skirt", &c.skirt, 0.05f, 0.0f, 20.0f, "%.2f m");
  track("Terrain skirt");
  changed |= ImGui::SliderFloat("Roughness", &c.roughness, 0.04f, 1.0f);
  track("Terrain roughness");
  changed |= ImGui::SliderFloat("Foliage distance", &c.foliage_distance, 0.1f, 4.0f, "x%.2f");
  track("Foliage distance");
  if (ImGui::Checkbox("Collider (Jolt heightfield)", &c.collider)) now("Terrain collider");
  changed |= ImGui::SliderFloat("Friction", &c.friction, 0.0f, 2.0f);
  track("Terrain friction");

  // Whole-terrain operations (undoable with a data snapshot).
  static int hill_seed = 1;
  static float hill_amp = 8.0f, hill_feature = 32.0f;
  if (ImGui::TreeNode("Generate")) {
    ImGui::InputInt("Seed", &hill_seed);
    ImGui::DragFloat("Amplitude", &hill_amp, 0.1f, 0.0f, 500.0f, "%.1f m");
    ImGui::DragFloat("Feature size", &hill_feature, 0.5f, 2.0f, 4000.0f, "%.0f m");
    const bool hills = ImGui::Button("Generate hills");
    ImGui::SameLine();
    const bool flat = ImGui::Button("Flatten all");
    ImGui::SameLine();
    const bool clear = ImGui::Button("Clear foliage");
    if ((hills || flat || clear) && d && d->valid()) {
      terrain::TerrainData before = *d;
      before.edits.clear();
      if (hills) terrain::generate_hills(*d, static_cast<std::uint32_t>(hill_seed), hill_amp, hill_feature);
      if (flat) {
        std::fill(d->heights.begin(), d->heights.end(), 0.0f);
        terrain::refit_foliage(*d, terrain::full_rect(*d));
        terrain::note_edit(*d, terrain::full_rect(*d), true, false, true);
      }
      if (clear) {
        d->foliage.clear();
        terrain::note_edit(*d, terrain::full_rect(*d), false, false, true);
      }
      std::vector<TerrainSnap> snaps;
      snaps.push_back({c, std::move(before)});
      push_terrain_undo(hills ? "Generate hills" : flat ? "Flatten terrain" : "Clear foliage", scene_,
                        std::move(snaps));
    }
    ImGui::TreePop();
  }
  if (view_) {
    const terrain::ViewStats& st = view_->terrain_view().stats();
    ImGui::TextDisabled("Drawn: %d chunks (LOD %d/%d/%d/%d), %zu plants", st.chunks, st.lod_counts[0],
                        st.lod_counts[1], st.lod_counts[2], st.lod_counts[3], st.foliage);
  }
  if (ImGui::CollapsingHeader("New terrain")) create_ui();

  // Removing a foliage type also drops its instances (snapshot for undo).
  if (remove_type >= 0 && d && d->valid()) {
    const scene3d::Scene3D before = scene_;
    terrain::TerrainData data = *d;
    data.edits.clear();
    terrain::Terrain3D old = c;
    c.foliage.erase(c.foliage.begin() + remove_type);
    terrain::remove_foliage_type(*d, remove_type);
    if (scene3d::Entity3D* live = scene_.find(e.id)) *live = e;
    std::vector<TerrainSnap> snaps;
    snaps.push_back({old, std::move(data)});
    push_terrain_undo("Remove foliage type", before, std::move(snaps));
    terrain_tool_.foliage_type = std::min(terrain_tool_.foliage_type, static_cast<int>(c.foliage.size()) - 1);
    terrain_edit_before_.reset();
    return;
  }
  if (changed || commit) {
    terrain::normalize(c);
    if (scene3d::Entity3D* live = scene_.find(e.id)) *live = e;  // live preview
  }
  if (commit && terrain_edit_before_) {
    const scene3d::Scene3D before = std::move(*terrain_edit_before_);
    terrain_edit_before_.reset();
    push_undo(commit, before);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
