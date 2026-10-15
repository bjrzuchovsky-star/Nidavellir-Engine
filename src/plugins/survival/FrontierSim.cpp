#include "plugins/survival/FrontierSim.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace survival {

namespace fs = std::filesystem;
using runtime::Aabb;
using runtime::World;

namespace {

constexpr double kDt = runtime::kTickSeconds;
constexpr float kFist = 2.0f;

// --- Tiny wire codec (little endian) -----------------------------------------
struct Out {
  std::vector<std::uint8_t> b;
  void u8(std::uint8_t v) { b.push_back(v); }
  void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); }
  void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
  void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void f32(float v) { std::uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
  void f64(double v) { std::uint64_t u; std::memcpy(&u, &v, 8); u64(u); }
};

struct In {
  const std::vector<std::uint8_t>& b;
  std::size_t p = 0;
  bool ok = true;
  explicit In(const std::vector<std::uint8_t>& bytes) : b(bytes) {}
  bool need(std::size_t n) { if (!ok || b.size() - p < n) ok = false; return ok; }
  std::uint8_t u8() { return need(1) ? b[p++] : 0; }
  std::uint16_t u16() { const std::uint16_t lo = u8(); return static_cast<std::uint16_t>(lo | (u8() << 8)); }
  std::uint32_t u32() { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i); return v; }
  std::uint64_t u64() { std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i); return v; }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  float f32() { const std::uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return std::isfinite(v) ? v : 0.0f; }
  double f64() { const std::uint64_t u = u64(); double v; std::memcpy(&v, &u, 8); return std::isfinite(v) ? v : 0.0; }
};

enum Msg : std::uint8_t {
  kWelcome = 1,
  kMsgChunk = 2,
  kEdits = 3,
  kState = 4,
  kInventory = 5,
  kSelect = 10,
  kCraft = 11,
  kSwap = 12,
};

void dir_vec(runtime::Facing f, float* dx, float* dy) {
  *dx = *dy = 0.0f;
  switch (f) {
    case runtime::Facing::Up: *dy = -1.0f; break;
    case runtime::Facing::Left: *dx = -1.0f; break;
    case runtime::Facing::Right: *dx = 1.0f; break;
    case runtime::Facing::Down: *dy = 1.0f; break;
  }
}

int tile_of(float px) { return static_cast<int>(std::floor(px / kTile)); }

// The first tile wholly past the rider's collider in the facing direction
// (so placing there never overlaps the rider), centred across.
void front_tile(const runtime::Aabb& b, runtime::Facing f, int* tx, int* ty) {
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  *tx = tile_of(cx);
  *ty = tile_of(cy);
  switch (f) {
    case runtime::Facing::Right: *tx = tile_of(b.x1 - 0.01f) + 1; break;
    case runtime::Facing::Left: *tx = tile_of(b.x0 + 0.01f) - 1; break;
    case runtime::Facing::Down: *ty = tile_of(b.y1 - 0.01f) + 1; break;
    case runtime::Facing::Up: *ty = tile_of(b.y0 + 0.01f) - 1; break;
  }
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------------

bool FrontierSim::begin(World& world, const std::string& project_dir, const GameData& data,
                        bool save, std::string* error) {
  data_ = data;
  project_dir_ = project_dir;
  mirror_ = false;
  saving_ = save && !project_dir.empty();
  save_dir_ = saving_ ? (fs::path(project_dir) / "saves" / data_.world.world).string() : "";
  day_len_ = static_cast<double>(data_.world.day_minutes) * 60.0;
  clock_ = day_len_ * 0.08;  // morning
  players_ = {};
  mobs_.clear();
  shots_.clear();
  homes_.clear();
  warnings_.clear();
  kills_ = spawned_total_ = edits_sent_ = 0;
  ticks_ = 0;
  rng_ = data_.world.seed * 0x9e3779b97f4a7c15ull + 1;
  std::uint64_t seed = data_.world.seed;
  chunks_.reset(seed, save_dir_);
  if (saving_ && !load_save(error)) return false;
  seeded_ = true;
  active_ = true;
  // Starter kit for a fresh claim.
  for (int s = 0; s < kSlots; ++s) {
    Player& p = players_[static_cast<std::size_t>(s)];
    if (p.inv.slots() == Inventory().slots()) {
      give(s, data_.item("torch"), 3);
      give(s, data_.item("cactus_flesh"), 2);
    }
    if (const runtime::Actor* a = world.player(s)) {
      spawn_[static_cast<std::size_t>(s)] = {a->data.x, a->data.y};
    }
  }
  return true;
}

void FrontierSim::begin_client(const GameData& data) {
  data_ = data;
  mirror_ = true;
  saving_ = false;
  save_dir_.clear();
  players_ = {};
  mobs_.clear();
  shots_.clear();
  seeded_ = false;
  active_ = true;
  day_len_ = static_cast<double>(data_.world.day_minutes) * 60.0;
}

void FrontierSim::end() {
  if (active_ && !mirror_ && saving_) {
    chunks_.save_all();
    write_save();
  }
  active_ = false;
  seeded_ = false;
}

bool FrontierSim::load_save(std::string* error) {
  const std::string text = read_text(fs::path(save_dir_) / "world.json");
  if (text.empty()) return true;  // a fresh claim
  json::Value v;
  std::string err;
  if (!json::parse(text, &v, &err)) {
    if (error) *error = "saves/" + data_.world.world + "/world.json: " + err;
    return false;
  }
  const auto seed = static_cast<std::uint64_t>(v.num("seed", static_cast<double>(data_.world.seed)));
  if (seed != data_.world.seed) {
    warnings_.push_back("the save was staked with seed " + std::to_string(seed) +
                        "; riding that (world.json says " + std::to_string(data_.world.seed) +
                        ", burn the save in the Prospector to switch)");
    data_.world.seed = seed;
    chunks_.reset(seed, save_dir_);
  }
  clock_ = v.num("clock", clock_);
  for (const json::Value& pv : v["players"].items()) {
    const int s = static_cast<int>(pv.num("slot", -1));
    if (s < 0 || s >= kSlots) continue;
    Player& p = players_[static_cast<std::size_t>(s)];
    p.hp = std::clamp(static_cast<int>(pv.num("hp", kMaxHp)), 1, kMaxHp);
    p.hotbar = std::clamp(static_cast<int>(pv.num("hotbar", 0)), 0, Inventory::kHotbar - 1);
    p.deaths = static_cast<int>(pv.num("deaths", 0));
    const json::Value& items = pv["items"];
    for (std::size_t i = 0; i < items.size() && i < Inventory::kSlots; ++i) {
      const json::Value& st = items[i];
      const int idx = data_.item(st.str("id"));
      const int n = static_cast<int>(st.num("n", 0));
      if (idx >= 0 && n > 0) p.inv.at(static_cast<int>(i)) = Stack{idx, n};
    }
  }
  for (const json::Value& h : v["homes"].items()) {
    homes_.push_back({static_cast<int>(h[0].as_number(0)), static_cast<int>(h[1].as_number(0))});
  }
  for (const auto& [tx, ty] : homes_) {
    const int c = data_.creature("settler");
    if (c >= 0) {
      const std::uint32_t id = spawn(c, (static_cast<float>(tx) + 0.5f) * kTile,
                                     (static_cast<float>(ty) + 0.5f) * kTile);
      for (Mob& m : mobs_) {
        if (m.id == id) {
          m.settler = true;
          m.home_tx = tx;
          m.home_ty = ty;
        }
      }
    }
  }
  return true;
}

bool FrontierSim::write_save() const {
  if (save_dir_.empty()) return false;
  json::Value v = json::Value::object();
  v.set("seed", json::Value::number(static_cast<double>(chunks_.seed())));
  v.set("gen_version", json::Value::number(kGenVersion));
  v.set("clock", json::Value::number(clock_));
  json::Value players = json::Value::array();
  for (int s = 0; s < kSlots; ++s) {
    const Player& p = players_[static_cast<std::size_t>(s)];
    json::Value pv = json::Value::object();
    pv.set("slot", json::Value::number(s));
    pv.set("hp", json::Value::number(p.hp));
    pv.set("hotbar", json::Value::number(p.hotbar));
    pv.set("deaths", json::Value::number(p.deaths));
    json::Value items = json::Value::array();
    for (const Stack& st : p.inv.slots()) {
      json::Value sv = json::Value::object();
      const ItemDef* d = data_.item_def(st.item);
      sv.set("id", json::Value::string(d && !st.empty() ? d->id : ""));
      sv.set("n", json::Value::number(d && !st.empty() ? st.count : 0));
      items.push(std::move(sv));
    }
    pv.set("items", std::move(items));
    players.push(std::move(pv));
  }
  v.set("players", std::move(players));
  json::Value homes = json::Value::array();
  for (const auto& [x, y] : homes_) {
    json::Value h = json::Value::array();
    h.push(json::Value::number(x));
    h.push(json::Value::number(y));
    homes.push(std::move(h));
  }
  v.set("homes", std::move(homes));
  std::error_code ec;
  fs::create_directories(save_dir_, ec);
  const fs::path path = fs::path(save_dir_) / "world.json";
  const fs::path tmp = fs::path(save_dir_) / "world.json.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json::write(v);
    if (!out) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
  }
  return !ec;
}

// --- Helpers ---------------------------------------------------------------------

std::uint32_t FrontierSim::rand() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 7;
  rng_ ^= rng_ << 17;
  return static_cast<std::uint32_t>(rng_ >> 11);
}

double FrontierSim::phase() const {
  if (day_len_ <= 0.0) return 0.25;
  return std::fmod(clock_, day_len_) / day_len_;
}

bool FrontierSim::night() const {
  const double p = phase();
  return p >= 0.66 && p < 0.97;
}

float FrontierSim::darkness() const {
  const double p = phase();
  double d = 0.0;
  if (p >= 0.58 && p < 0.68) d = (p - 0.58) / 0.10;
  else if (p >= 0.68 && p < 0.94) d = 1.0;
  else if (p >= 0.94) d = 1.0 - (p - 0.94) / 0.06;
  return static_cast<float>(d * 0.62);
}

std::size_t FrontierSim::settlers() const {
  std::size_t n = 0;
  for (const Mob& m : mobs_) n += m.settler ? 1 : 0;
  return n;
}

void FrontierSim::toast(World& w, int slot, const std::string& text, double seconds) {
  w.script_toast(slot, text, seconds, 0);
}

void FrontierSim::hint(World& w, int slot, const std::string& text) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  if (p.toast_cd > 0.0) return;
  p.toast_cd = 2.0;
  toast(w, slot, text, 1.8);
}

void FrontierSim::sound(World& w, const char* clip, float x, float y, float volume,
                        float pitch, float variation) {
  runtime::SoundEvent ev;
  ev.clip = clip;
  ev.volume = volume;
  ev.pitch = pitch;
  ev.variation = variation;
  ev.positional = true;
  ev.x = x;
  ev.y = y;
  w.play_sound(std::move(ev));
}

void FrontierSim::burst(World& w, const char* preset, float x, float y, int count, float dir) {
  runtime::FxEvent ev;
  ev.preset = preset;
  ev.x = x;
  ev.y = y;
  ev.count = count;
  ev.dir = dir;
  w.fx(std::move(ev));
}

// One footfall every ~26 px walked (a rider standing still is quiet).
void FrontierSim::footsteps(World& w, int slot) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  Aabb b;
  if (!rider_box(w, slot, &b)) return;
  const float x = (b.x0 + b.x1) * 0.5f;
  const float y = b.y1;
  if (!p.stepped) {
    p.step_x = x;
    p.step_y = y;
    p.stepped = true;
    return;
  }
  const float dx = x - p.step_x;
  const float dy = y - p.step_y;
  const float d2 = dx * dx + dy * dy;
  if (d2 > 200.0f * 200.0f) {  // teleported / respawned
    p.step_x = x;
    p.step_y = y;
    return;
  }
  if (d2 < 26.0f * 26.0f) return;
  p.step_x = x;
  p.step_y = y;
  sound(w, "builtin:step", x, y, 0.45f, 1.0f, 0.15f);
}

bool FrontierSim::rider_box(const World& w, int slot, Aabb* box) const {
  const runtime::Actor* a = w.player(slot);
  if (!a || a->hidden) return false;
  *box = runtime::collider_box(a->data);
  return true;
}

bool FrontierSim::box_blocked(float x0, float y0, float x1, float y1) const {
  for (int ty = tile_of(y0); ty <= tile_of(y1 - 0.01f); ++ty) {
    for (int tx = tile_of(x0); tx <= tile_of(x1 - 0.01f); ++tx) {
      const Tile* t = chunks_.peek(tx, ty);
      if (!t || tile_solid(*t)) return true;
    }
  }
  return false;
}

bool FrontierSim::walkable_px(float x, float y, float half) const {
  return !box_blocked(x - half, y - half, x + half, y + half);
}

bool FrontierSim::box_has_body(const World& w, float x0, float y0, float x1, float y1) const {
  const Aabb t{x0, y0, x1, y1};
  for (int s = 0; s < kSlots; ++s) {
    Aabb b;
    if (rider_box(w, s, &b) && runtime::overlaps(b, t)) return true;
  }
  for (const Mob& m : mobs_) {
    const float h = data_.creatures[static_cast<std::size_t>(m.def)].size * kTile * 0.4f;
    if (runtime::overlaps(Aabb{m.x - h, m.y - h, m.x + h, m.y + h}, t)) return true;
  }
  return false;
}

void FrontierSim::place_safe(World& w, int slot) {
  const runtime::Actor* cur = w.player(slot);
  if (!cur) return;
  runtime::Actor* a = w.find(cur->data.id);
  Aabb b = runtime::collider_box(a->data);
  if (!box_blocked(b.x0, b.y0, b.x1, b.y1)) return;
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  const int tx0 = tile_of(cx), ty0 = tile_of(cy);
  for (int r = 1; r < 24; ++r) {
    for (int dy = -r; dy <= r; ++dy) {
      for (int dx = -r; dx <= r; ++dx) {
        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
        const float nx = (static_cast<float>(tx0 + dx) + 0.5f) * kTile;
        const float ny = (static_cast<float>(ty0 + dy) + 0.5f) * kTile;
        const float ox = nx - cx, oy = ny - cy;
        if (!box_blocked(b.x0 + ox, b.y0 + oy, b.x1 + ox, b.y1 + oy)) {
          a->data.x += ox;
          a->data.y += oy;
          a->prev_x = a->data.x;
          a->prev_y = a->data.y;
          return;
        }
      }
    }
  }
}

// --- The tick ---------------------------------------------------------------------

void FrontierSim::step(World& w) {
  if (!active_ || mirror_) return;
  ++ticks_;
  clock_ += kDt;
  const bool server = w.net_role() == runtime::NetRole::Server;

  std::vector<std::pair<float, float>> centres;
  for (int s = 0; s < kSlots; ++s) {
    Player& p = players_[static_cast<std::size_t>(s)];
    Aabb b;
    const bool here = rider_box(w, s, &b);
    if (here) centres.push_back({(b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f});
    if (here && !p.present) {
      p.present = true;
      p.sent.clear();
      p.inv_dirty = true;
      chunks_.stream({centres.back()});
      place_safe(w, s);
      if (server) send_welcome(w, s);
    } else if (!here) {
      p.present = false;
    }
  }
  chunks_.stream(centres);

  const runtime::InputFrame& in = w.last_input();
  for (int s = 0; s < kSlots; ++s) {
    Player& p = players_[static_cast<std::size_t>(s)];
    if (!p.present) continue;
    p.cooldown = std::max(0.0, p.cooldown - kDt);
    p.hurt = std::max(0.0, p.hurt - kDt);
    p.toast_cd = std::max(0.0, p.toast_cd - kDt);
    act(w, s, in.slot(s).buttons);
    footsteps(w, s);
  }
  step_mobs(w);
  step_shots(w);
  if (ticks_ % 120 == 0) {
    for (int s = 0; s < kSlots; ++s) {
      if (players_[static_cast<std::size_t>(s)].present) spawn_near(w, s);
    }
  }

  std::vector<TileEdit> edits = chunks_.take_edits();
  if (server) {
    if (!edits.empty()) {
      Out o;
      o.u8(kEdits);
      o.u16(static_cast<std::uint16_t>(std::min<std::size_t>(edits.size(), 4096)));
      for (std::size_t i = 0; i < edits.size() && i < 4096; ++i) {
        o.i32(edits[i].tx);
        o.i32(edits[i].ty);
        o.u8(edits[i].tile.ground);
        o.u8(edits[i].tile.object);
        o.u8(edits[i].tile.damage);
      }
      edits_sent_ += edits.size();
      post(w, -1, std::move(o.b));
    }
    for (int s = 0; s < kSlots; ++s) {
      Player& p = players_[static_cast<std::size_t>(s)];
      if (!p.present) continue;
      if (p.inv_dirty) send_inventory(w, s);
      if (ticks_ % 10 == 0) sync_chunks(w, s);
    }
    if (ticks_ % kStateEvery == 0) post(w, -1, encode_state());
  }
  if (saving_ && clock_ >= autosave_at_) {
    autosave_at_ = clock_ + 60.0;
    chunks_.save_all();
    write_save();
  }
}

void FrontierSim::act(World& w, int slot, std::uint32_t buttons) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  const std::uint32_t pressed = buttons & ~p.prev_buttons;
  p.prev_buttons = buttons;
  if (pressed & runtime::kButtonNext) p.hotbar = (p.hotbar + 1) % Inventory::kHotbar;
  if (pressed & runtime::kButtonPrev) p.hotbar = (p.hotbar + Inventory::kHotbar - 1) % Inventory::kHotbar;

  const runtime::Actor* a = w.player(slot);
  const Aabb b = runtime::collider_box(a->data);
  front_tile(b, a->dir, &p.target_tx, &p.target_ty);

  const Stack& held = p.inv.at(p.hotbar);
  const ItemDef* item = held.empty() ? nullptr : data_.item_def(held.item);
  if ((buttons & runtime::kButtonUse) && p.cooldown <= 0.0) {
    if (item && item->kind == ItemKind::Food) {
      p.hp = std::min(p.max_hp, p.hp + item->heal);
      toast(w, slot, "Ate " + item->name + " (+" + std::to_string(item->heal) + " grit)", 1.5);
      p.inv.remove(held.item, 1);
      p.inv_dirty = true;
      p.cooldown = 0.5;
    } else if (!melee(w, slot, item)) {
      mine(w, slot, item);
    }
  }
  if (pressed & runtime::kButtonPlace) place(w, slot);
  if ((buttons & runtime::kButtonFire) && p.cooldown <= 0.0) fire(w, slot, item);
}

bool FrontierSim::melee(World& w, int slot, const ItemDef* item) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  const runtime::Actor* a = w.player(slot);
  const Aabb b = runtime::collider_box(a->data);
  float dx, dy;
  dir_vec(a->dir, &dx, &dy);
  const bool armed = item && (item->kind == ItemKind::Weapon || item->kind == ItemKind::Tool);
  const float reach = (armed ? item->reach : 1.2f) * kTile;
  const float cx = (b.x0 + b.x1) * 0.5f + dx * reach * 0.5f;
  const float cy = (b.y0 + b.y1) * 0.5f + dy * reach * 0.5f;
  const Aabb hit{cx - reach * 0.55f, cy - reach * 0.55f, cx + reach * 0.55f, cy + reach * 0.55f};
  const int damage = armed && item->damage > 0 ? item->damage : static_cast<int>(kFist);
  bool any = false;
  for (std::size_t i = 0; i < mobs_.size(); ++i) {
    Mob& m = mobs_[i];
    const CreatureDef& c = data_.creatures[static_cast<std::size_t>(m.def)];
    if (c.friendly) continue;
    const float h = c.size * kTile * 0.4f;
    if (!runtime::overlaps(Aabb{m.x - h, m.y - h, m.x + h, m.y + h}, hit)) continue;
    m.hp -= damage;
    m.hurt = 0.25f;
    m.x += dx * 10.0f;
    m.y += dy * 10.0f;
    m.state = 1;
    any = true;
  }
  if (!any) return false;
  p.cooldown = armed ? item->cooldown : 0.35;
  sound(w, "builtin:mine", cx, cy, 0.8f, 1.35f);
  for (std::size_t i = mobs_.size(); i-- > 0;) {
    if (mobs_[i].hp <= 0) kill(w, i, slot);
  }
  return true;
}

void FrontierSim::mine(World& w, int slot, const ItemDef* item) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  Tile t = chunks_.get(p.target_tx, p.target_ty);
  const bool tool = item && item->kind == ItemKind::Tool;
  const int power = tool ? std::max(1, item->mine_power) : 1;
  const int tier = tool ? item->tier : 0;
  p.cooldown = tool ? item->cooldown : 0.35;
  const float tx = (static_cast<float>(p.target_tx) + 0.5f) * kTile;
  const float ty = (static_cast<float>(p.target_ty) + 0.5f) * kTile;
  if (t.object != kObjNone) {
    const ObjectDef& od = object_def(t.object);
    if (tier < od.min_tier) {
      hint(w, slot, std::string(od.name) + " is too tough for " +
                        (tool ? "that " + item->name : std::string("bare hands")) + ".");
      return;
    }
    const int dmg = t.damage + power;
    if (dmg < od.hardness) {
      t.damage = static_cast<std::uint8_t>(std::min(dmg, 255));
      chunks_.set(p.target_tx, p.target_ty, t);
      sound(w, "builtin:mine", tx, ty, 0.8f);
      burst(w, "dust", tx, ty, 5);
      burst(w, "sparks", tx, ty, 4);
      return;
    }
    // Broke it: a lower, heavier knock.
    sound(w, "builtin:mine", tx, ty, 1.0f, 0.7f);
    burst(w, "dust", tx, ty, 14);  // debris
    const std::uint8_t broken = t.object;
    t.object = kObjNone;
    t.damage = 0;
    chunks_.set(p.target_tx, p.target_ty, t);
    if (broken == kBed) {
      homes_.erase(std::remove(homes_.begin(), homes_.end(), std::make_pair(p.target_tx, p.target_ty)),
                   homes_.end());
      for (Mob& m : mobs_) {
        if (m.settler && m.home_tx == p.target_tx && m.home_ty == p.target_ty) m.hp = 0;
      }
      mobs_.erase(std::remove_if(mobs_.begin(), mobs_.end(), [](const Mob& m) { return m.hp <= 0; }),
                  mobs_.end());
    }
    const int drop = data_.item(od.drop);
    if (drop >= 0) {
      const int left = p.inv.add(data_, drop, od.drop_count);
      p.inv_dirty = true;
      if (left > 0) hint(w, slot, "Saddlebags are full; " + data_.items[static_cast<std::size_t>(drop)].name + " left in the dirt.");
    }
    return;
  }
  if (t.ground == kPlanks) {
    sound(w, "builtin:mine", tx, ty, 0.8f, 0.85f);
    burst(w, "dust", tx, ty, 8);
    t.ground = kDirt;
    chunks_.set(p.target_tx, p.target_ty, t);
    const int drop = data_.item("planks");
    if (drop >= 0) p.inv.add(data_, drop, 1);
    p.inv_dirty = true;
  }
}

void FrontierSim::place(World& w, int slot) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  const Stack held = p.inv.at(p.hotbar);
  const ItemDef* item = held.empty() ? nullptr : data_.item_def(held.item);
  if (!item || item->kind != ItemKind::Placeable) return;
  const int tx = p.target_tx, ty = p.target_ty;
  Tile t = chunks_.get(tx, ty);
  if (item->places_ground != kGroundNone) {
    if (!ground_def(t.ground).walkable || t.ground == item->places_ground) return;
    t.ground = item->places_ground;
  } else {
    if (t.object != kObjNone || !ground_def(t.ground).walkable) return;
    const float x0 = static_cast<float>(tx) * kTile, y0 = static_cast<float>(ty) * kTile;
    if (object_def(item->places_object).solid && box_has_body(w, x0, y0, x0 + kTile, y0 + kTile)) {
      hint(w, slot, "Somebody's standing there.");
      return;
    }
    t.object = item->places_object;
    t.damage = 0;
  }
  chunks_.set(tx, ty, t);
  p.inv.remove(held.item, 1);
  p.inv_dirty = true;
  if (item->places_object == kBed) settle(w, slot, tx, ty);
}

void FrontierSim::fire(World& w, int slot, const ItemDef* item) {
  if (!item || item->kind != ItemKind::Ranged) return;
  Player& p = players_[static_cast<std::size_t>(slot)];
  const int ammo = data_.item(item->ammo);
  if (ammo >= 0 && p.inv.remove(ammo, 1) == 0) {
    hint(w, slot, "Out of " + data_.items[static_cast<std::size_t>(ammo)].name + ".");
    p.cooldown = 0.3;
    return;
  }
  p.inv_dirty = true;
  const runtime::Actor* a = w.player(slot);
  const Aabb b = runtime::collider_box(a->data);
  float dx, dy;
  dir_vec(a->dir, &dx, &dy);
  Shot s;
  s.x = (b.x0 + b.x1) * 0.5f + dx * 12.0f;
  s.y = (b.y0 + b.y1) * 0.5f + dy * 12.0f;
  s.vx = dx * kShotSpeed;
  s.vy = dy * kShotSpeed;
  s.damage = item->damage;
  s.owner = slot;
  shots_.push_back(s);
  p.cooldown = item->cooldown;
  sound(w, "builtin:gunshot", s.x, s.y, 0.9f, 1.0f, 0.08f);
  const float aim = std::atan2(dy, dx) * 57.2957795f;
  burst(w, "muzzle_flash", s.x, s.y, 0, aim);
  burst(w, "gun_smoke", s.x, s.y, 6, aim);
}

void FrontierSim::step_shots(World& w) {
  for (std::size_t i = 0; i < shots_.size();) {
    Shot& s = shots_[i];
    s.x += s.vx * static_cast<float>(kDt);
    s.y += s.vy * static_cast<float>(kDt);
    s.life -= static_cast<float>(kDt);
    bool gone = s.life <= 0.0f;
    const Tile* t = chunks_.peek(tile_of(s.x), tile_of(s.y));
    if (!t || object_def(t->object).solid) gone = true;
    for (std::size_t m = 0; !gone && m < mobs_.size(); ++m) {
      Mob& mob = mobs_[m];
      const CreatureDef& c = data_.creatures[static_cast<std::size_t>(mob.def)];
      if (c.friendly) continue;
      const float h = c.size * kTile * 0.45f;
      if (std::fabs(mob.x - s.x) < h && std::fabs(mob.y - s.y) < h) {
        mob.hp -= s.damage;
        mob.hurt = 0.25f;
        mob.state = 1;
        gone = true;
        if (mob.hp <= 0) kill(w, m, s.owner);
      }
    }
    if (gone) shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
  }
}

void FrontierSim::kill(World& w, std::size_t mob, int killer) {
  if (mob >= mobs_.size()) return;
  const Mob m = mobs_[mob];
  const CreatureDef& c = data_.creatures[static_cast<std::size_t>(m.def)];
  mobs_.erase(mobs_.begin() + static_cast<std::ptrdiff_t>(mob));
  ++kills_;
  if (killer >= 0 && killer < kSlots && c.drop >= 0) {
    Player& p = players_[static_cast<std::size_t>(killer)];
    p.inv.add(data_, c.drop, c.drop_count);
    p.inv_dirty = true;
    toast(w, killer, c.name + " down. Took " + data_.items[static_cast<std::size_t>(c.drop)].name + ".",
          1.8);
  } else if (killer >= 0 && killer < kSlots) {
    toast(w, killer, c.name + " down.", 1.5);
  }
}

void FrontierSim::hurt_player(World& w, int slot, int damage, const std::string& by) {
  if (slot < 0 || slot >= kSlots) return;
  Player& p = players_[static_cast<std::size_t>(slot)];
  if (!p.present || damage <= 0) return;
  p.hp -= damage;
  p.hurt = 0.6;
  if (p.hp > 0) return;
  ++p.deaths;
  p.hp = p.max_hp;
  toast(w, slot, "Bushwhacked by " + by + ". Woke up back at camp.", 3.0);
  if (const runtime::Actor* cur = w.player(slot)) {
    runtime::Actor* a = w.find(cur->data.id);
    a->data.x = spawn_[static_cast<std::size_t>(slot)].first;
    a->data.y = spawn_[static_cast<std::size_t>(slot)].second;
    a->prev_x = a->data.x;
    a->prev_y = a->data.y;
    chunks_.stream({{a->data.x, a->data.y}});
    place_safe(w, slot);
  }
}

void FrontierSim::heal_player(int slot, int n) {
  if (slot < 0 || slot >= kSlots) return;
  Player& p = players_[static_cast<std::size_t>(slot)];
  p.hp = std::clamp(p.hp + n, 0, p.max_hp);
}

// --- Creatures ----------------------------------------------------------------------

std::uint32_t FrontierSim::spawn(int creature, float x, float y) {
  if (creature < 0 || creature >= static_cast<int>(data_.creatures.size())) return 0;
  Mob m;
  m.id = next_mob_++;
  m.def = creature;
  m.x = x;
  m.y = y;
  m.hp = data_.creatures[static_cast<std::size_t>(creature)].hp;
  m.timer = 0.5f;
  m.settler = data_.creatures[static_cast<std::size_t>(creature)].friendly;
  m.home_tx = tile_of(x);
  m.home_ty = tile_of(y);
  mobs_.push_back(m);
  ++spawned_total_;
  return m.id;
}

void FrontierSim::spawn_near(World& w, int slot) {
  Aabb b;
  if (!rider_box(w, slot, &b)) return;
  const float px = (b.x0 + b.x1) * 0.5f, py = (b.y0 + b.y1) * 0.5f;
  int hostiles = 0;
  for (const Mob& m : mobs_) {
    if (!m.settler && std::hypot(m.x - px, m.y - py) < 32.0f * kTile) ++hostiles;
  }
  const bool dark = night();
  if (hostiles >= (dark ? 8 : 4)) return;
  for (int attempt = 0; attempt < 6; ++attempt) {
    const float ang = randf() * 6.2831853f;
    const float dist = (10.0f + randf() * 6.0f) * kTile;
    const float x = px + std::cos(ang) * dist, y = py + std::sin(ang) * dist;
    const int tx = tile_of(x), ty = tile_of(y);
    const Tile* t = chunks_.peek(tx, ty);
    if (!t || tile_solid(*t) || t->ground == kPlanks) continue;
    const Biome bio = chunks_.gen().biome_at(tx, ty);
    int total = 0;
    for (const CreatureDef& c : data_.creatures) {
      if (!c.friendly && c.biomes[static_cast<int>(bio)] && (!c.night_only || dark)) total += c.weight;
    }
    if (total <= 0) continue;
    int pick = static_cast<int>(rand() % static_cast<std::uint32_t>(total));
    for (std::size_t i = 0; i < data_.creatures.size(); ++i) {
      const CreatureDef& c = data_.creatures[i];
      if (c.friendly || !c.biomes[static_cast<int>(bio)] || (c.night_only && !dark)) continue;
      pick -= c.weight;
      if (pick < 0) {
        spawn(static_cast<int>(i), (static_cast<float>(tx) + 0.5f) * kTile,
              (static_cast<float>(ty) + 0.5f) * kTile);
        return;
      }
    }
  }
}

namespace {
FrontierSim::MobSteer& mob_steer() {
  static FrontierSim::MobSteer s;
  return s;
}
}  // namespace

void FrontierSim::set_mob_steer(MobSteer steer) { mob_steer() = std::move(steer); }

void FrontierSim::step_mobs(World& w) {
  const float dt = static_cast<float>(kDt);
  const bool dark = night();
  for (std::size_t i = 0; i < mobs_.size();) {
    Mob& m = mobs_[i];
    const CreatureDef& c = data_.creatures[static_cast<std::size_t>(m.def)];
    const float half = c.size * kTile * 0.4f;
    m.hurt = std::max(0.0f, m.hurt - dt);
    m.cd = std::max(0.0f, m.cd - dt);
    m.timer -= dt;
    // Nearest rider.
    int target = -1;
    float best = 1e30f, tx = 0.0f, ty = 0.0f;
    for (int s = 0; s < kSlots; ++s) {
      Aabb b;
      if (!rider_box(w, s, &b)) continue;
      const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
      const float d = std::hypot(cx - m.x, cy - m.y);
      if (d < best) {
        best = d;
        target = s;
        tx = cx;
        ty = cy;
      }
    }
    const bool far = target < 0 || best > 48.0f * kTile;
    const bool loaded = chunks_.peek(tile_of(m.x), tile_of(m.y)) != nullptr;
    const bool dawn_rider = c.night_only && !dark && best > 12.0f * kTile;
    if (!m.settler && (far || !loaded || dawn_rider)) {
      mobs_.erase(mobs_.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    if (m.settler && !loaded) {
      ++i;
      continue;
    }
    float vx = 0.0f, vy = 0.0f;
    if (!c.friendly && target >= 0 && best < c.aggro * kTile) {
      m.state = 1;
      const float reach = half + 18.0f;
      if (best > reach) {
        float hx = (tx - m.x) / best, hy = (ty - m.y) / best;
        if (mob_steer()) {
          const auto walk = [this, half](float px, float py) { return walkable_px(px, py, half); };
          mob_steer()(w, m.id, m.x, m.y, tx, ty, half, walk, &hx, &hy);
        }
        vx = hx * c.speed;
        vy = hy * c.speed;
      } else if (m.cd <= 0.0f) {
        m.cd = 1.0f;
        Player& p = players_[static_cast<std::size_t>(target)];
        if (p.hurt <= 0.0) hurt_player(w, target, c.damage, c.name);
      }
    } else {
      if (m.timer <= 0.0f) {
        m.timer = 1.0f + randf() * 2.5f;
        if (randf() < 0.35f) {
          m.dx = m.dy = 0.0f;
        } else {
          const float ang = randf() * 6.2831853f;
          m.dx = std::cos(ang);
          m.dy = std::sin(ang);
        }
        if (m.settler) {
          // Stay home: head back when straying.
          const float hx = (static_cast<float>(m.home_tx) + 0.5f) * kTile;
          const float hy = (static_cast<float>(m.home_ty) + 0.5f) * kTile;
          const float d = std::hypot(hx - m.x, hy - m.y);
          if (d > 3.0f * kTile) {
            m.dx = (hx - m.x) / d;
            m.dy = (hy - m.y) / d;
          }
        }
      }
      m.state = 0;
      vx = m.dx * c.speed * 0.5f;
      vy = m.dy * c.speed * 0.5f;
    }
    const float nx = m.x + vx * dt;
    if (walkable_px(nx, m.y, half)) m.x = nx;
    else m.timer = 0.0f;
    const float ny = m.y + vy * dt;
    if (walkable_px(m.x, ny, half)) m.y = ny;
    else m.timer = 0.0f;
    ++i;
  }
}

// --- Settlers -----------------------------------------------------------------------

HouseCheck FrontierSim::check_house(int bed_tx, int bed_ty) {
  HouseCheck r;
  constexpr int kMaxArea = 64;
  std::vector<std::pair<int, int>> open{{bed_tx, bed_ty}};
  std::set<std::pair<int, int>> seen{{bed_tx, bed_ty}};
  std::set<std::pair<int, int>> doors;
  while (!open.empty()) {
    const auto [x, y] = open.back();
    open.pop_back();
    ++r.area;
    if (r.area > kMaxArea) {
      r.why = "the room is open to the prairie (or bigger than " + std::to_string(kMaxArea) + " tiles)";
      return r;
    }
    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : nb) {
      const std::pair<int, int> n{x + d[0], y + d[1]};
      if (seen.count(n)) continue;
      const Tile* t = chunks_.peek(n.first, n.second);
      if (!t) {
        r.why = "the room runs off the map";
        return r;
      }
      if (t->object == kDoor) {
        doors.insert(n);
        continue;
      }
      if (tile_solid(*t)) continue;
      seen.insert(n);
      open.push_back(n);
    }
  }
  r.doors = static_cast<int>(doors.size());
  if (r.area < 4) {
    r.why = "too cramped (" + std::to_string(r.area) + " tiles; 4 or more)";
  } else if (r.doors == 0) {
    r.why = "no door to come and go by";
  } else {
    r.ok = true;
  }
  return r;
}

void FrontierSim::settle(World& w, int slot, int tx, int ty) {
  const HouseCheck h = check_house(tx, ty);
  if (!h.ok) {
    toast(w, slot, "Bedroll laid, but no settler will take it: " + h.why + ".", 3.0);
    return;
  }
  const int c = data_.creature("settler");
  if (c < 0) return;
  const std::uint32_t id = spawn(c, (static_cast<float>(tx) + 0.5f) * kTile,
                                 (static_cast<float>(ty) + 0.5f) * kTile);
  for (Mob& m : mobs_) {
    if (m.id == id) {
      m.settler = true;
      m.home_tx = tx;
      m.home_ty = ty;
    }
  }
  homes_.push_back({tx, ty});
  toast(w, slot, "A settler moved in (" + std::to_string(h.area) + " tiles, " +
                     std::to_string(h.doors) + " door" + (h.doors == 1 ? "" : "s") +
                     "). Jobs: coming soon.", 3.0);
}

// --- Actions ---------------------------------------------------------------------------

int FrontierSim::give(int slot, int item, int n) {
  if (slot < 0 || slot >= kSlots || !data_.item_def(item)) return n;
  Player& p = players_[static_cast<std::size_t>(slot)];
  p.inv_dirty = true;
  return p.inv.add(data_, item, n);
}

int FrontierSim::take(int slot, int item, int n) {
  if (slot < 0 || slot >= kSlots) return 0;
  Player& p = players_[static_cast<std::size_t>(slot)];
  p.inv_dirty = true;
  return p.inv.remove(item, n);
}

bool FrontierSim::near_station(int slot, std::uint8_t station) const {
  if (station == kObjNone) return true;
  const Player& p = players_[static_cast<std::size_t>(slot)];
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      const Tile* t = chunks_.peek(p.target_tx + dx, p.target_ty + dy);
      if (t && t->object == station) return true;
    }
  }
  return false;
}

bool FrontierSim::can_craft(int slot, int recipe, std::string* why) const {
  if (slot < 0 || slot >= kSlots || recipe < 0 || recipe >= static_cast<int>(data_.recipes.size())) {
    if (why) *why = "no such recipe";
    return false;
  }
  const RecipeDef& r = data_.recipes[static_cast<std::size_t>(recipe)];
  const Player& p = players_[static_cast<std::size_t>(slot)];
  for (const auto& [item, n] : r.needs) {
    if (!p.inv.has(item, n)) {
      if (why) *why = "short on " + data_.items[static_cast<std::size_t>(item)].name;
      return false;
    }
  }
  if (!near_station(slot, r.station)) {
    if (why) *why = std::string("needs a ") + object_def(r.station).name + " close by";
    return false;
  }
  return true;
}

bool FrontierSim::craft(World& w, int slot, int recipe, std::string* why) {
  if (mirror_) {
    Out o;
    o.u8(kCraft);
    o.u16(static_cast<std::uint16_t>(recipe));
    request(w, std::move(o.b));
    return true;
  }
  if (!can_craft(slot, recipe, why)) return false;
  const RecipeDef& r = data_.recipes[static_cast<std::size_t>(recipe)];
  Player& p = players_[static_cast<std::size_t>(slot)];
  Inventory trial = p.inv;
  for (const auto& [item, n] : r.needs) trial.remove(item, n);
  if (trial.add(data_, r.makes, r.count) > 0) {
    if (why) *why = "no room in the saddlebags";
    return false;
  }
  p.inv = trial;
  p.inv_dirty = true;
  toast(w, slot, "Made " + std::to_string(r.count) + " " + data_.items[static_cast<std::size_t>(r.makes)].name + ".", 1.5);
  return true;
}

void FrontierSim::select(World& w, int slot, int hotbar) {
  if (slot < 0 || slot >= kSlots) return;
  hotbar = std::clamp(hotbar, 0, Inventory::kHotbar - 1);
  if (mirror_) {
    players_[static_cast<std::size_t>(slot)].hotbar = hotbar;  // predicted
    Out o;
    o.u8(kSelect);
    o.u8(static_cast<std::uint8_t>(hotbar));
    request(w, std::move(o.b));
    return;
  }
  players_[static_cast<std::size_t>(slot)].hotbar = hotbar;
}

void FrontierSim::swap(World& w, int slot, int a, int b) {
  if (slot < 0 || slot >= kSlots) return;
  if (mirror_) {
    Out o;
    o.u8(kSwap);
    o.u8(static_cast<std::uint8_t>(a));
    o.u8(static_cast<std::uint8_t>(b));
    request(w, std::move(o.b));
    return;
  }
  Player& p = players_[static_cast<std::size_t>(slot)];
  p.inv.swap(a, b);
  p.inv_dirty = true;
}

bool FrontierSim::set_tile(int tx, int ty, const Tile& t) {
  if (mirror_ || !seeded_) return false;
  return chunks_.set(tx, ty, t);
}

Tile FrontierSim::tile(int tx, int ty) {
  if (!seeded_) return Tile{};
  if (mirror_) {
    const Tile* t = chunks_.peek(tx, ty);
    return t ? *t : chunks_.gen().tile_at(tx, ty);
  }
  return chunks_.get(tx, ty);
}

// --- Network -------------------------------------------------------------------------

void FrontierSim::post(World& w, int slot, std::vector<std::uint8_t> bytes) {
  w.net_post(kChannel, slot, std::move(bytes));
}

void FrontierSim::request(World& w, std::vector<std::uint8_t> bytes) {
  w.net_post(kChannel, -1, std::move(bytes));
}

void FrontierSim::send_welcome(World& w, int slot) {
  Out o;
  o.u8(kWelcome);
  o.u8(static_cast<std::uint8_t>(slot));
  o.u64(chunks_.seed());
  o.u64(data_.hash);
  o.f64(clock_);
  o.f64(day_len_);
  post(w, slot, std::move(o.b));
}

void FrontierSim::send_inventory(World& w, int slot) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  p.inv_dirty = false;
  Out o;
  o.u8(kInventory);
  o.u8(static_cast<std::uint8_t>(slot));
  o.u8(static_cast<std::uint8_t>(p.hotbar));
  for (const Stack& s : p.inv.slots()) {
    o.u16(static_cast<std::uint16_t>(s.empty() ? 0xFFFF : s.item));
    o.u16(static_cast<std::uint16_t>(s.empty() ? 0 : s.count));
  }
  post(w, slot, std::move(o.b));
}

void FrontierSim::sync_chunks(World& w, int slot) {
  Player& p = players_[static_cast<std::size_t>(slot)];
  Aabb b;
  if (!rider_box(w, slot, &b)) return;
  const int cx = floor_div(tile_of((b.x0 + b.x1) * 0.5f), kChunk);
  const int cy = floor_div(tile_of((b.y0 + b.y1) * 0.5f), kChunk);
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const Chunk* c = chunks_.find(cx + dx, cy + dy);
      if (!c || !c->edited || p.sent.count({c->cx, c->cy})) continue;
      p.sent.insert({c->cx, c->cy});
      Out o;
      o.u8(kMsgChunk);
      const std::vector<std::uint8_t> enc = ChunkMap::encode(*c, chunks_.seed());
      o.u32(static_cast<std::uint32_t>(enc.size()));
      o.b.insert(o.b.end(), enc.begin(), enc.end());
      post(w, slot, std::move(o.b));
    }
  }
}

std::vector<std::uint8_t> FrontierSim::encode_state() const {
  Out o;
  o.u8(kState);
  o.f64(clock_);
  for (const Player& p : players_) {
    o.u8(p.present ? 1 : 0);
    o.u16(static_cast<std::uint16_t>(std::max(0, p.hp)));
    o.u16(static_cast<std::uint16_t>(p.max_hp));
    o.u8(static_cast<std::uint8_t>(p.hotbar));
    o.u8(p.hurt > 0.0 ? 1 : 0);
  }
  const std::size_t nm = std::min<std::size_t>(mobs_.size(), 512);
  o.u16(static_cast<std::uint16_t>(nm));
  for (std::size_t i = 0; i < nm; ++i) {
    const Mob& m = mobs_[i];
    o.u32(m.id);
    o.u16(static_cast<std::uint16_t>(m.def));
    o.f32(m.x);
    o.f32(m.y);
    o.u16(static_cast<std::uint16_t>(std::max(0, m.hp)));
    o.u8(static_cast<std::uint8_t>((m.hurt > 0.0f ? 1 : 0) | (m.settler ? 2 : 0) | (m.state == 1 ? 4 : 0)));
  }
  const std::size_t ns = std::min<std::size_t>(shots_.size(), 256);
  o.u16(static_cast<std::uint16_t>(ns));
  for (std::size_t i = 0; i < ns; ++i) {
    o.f32(shots_[i].x);
    o.f32(shots_[i].y);
    o.f32(shots_[i].vx);
    o.f32(shots_[i].vy);
  }
  return o.b;
}

void FrontierSim::receive(World& w, int from_slot, const std::vector<std::uint8_t>& bytes) {
  if (!active_ || bytes.empty()) return;
  if (mirror_) {
    apply(bytes);
    return;
  }
  // Server: requests from a seated rider.
  if (from_slot < 0 || from_slot >= kSlots) return;
  In in(bytes);
  const std::uint8_t type = in.u8();
  if (type == kSelect) {
    const int h = in.u8();
    if (in.ok) select(w, from_slot, h);
    players_[static_cast<std::size_t>(from_slot)].inv_dirty = true;
  } else if (type == kCraft) {
    const int r = in.u16();
    std::string why;
    if (in.ok && !craft(w, from_slot, r, &why)) hint(w, from_slot, "Can't make that: " + why + ".");
  } else if (type == kSwap) {
    const int a = in.u8(), b = in.u8();
    if (in.ok) swap(w, from_slot, a, b);
  }
}

void FrontierSim::apply(const std::vector<std::uint8_t>& bytes) {
  In in(bytes);
  const std::uint8_t type = in.u8();
  switch (type) {
    case kWelcome: {
      const int slot = in.u8();
      const std::uint64_t seed = in.u64();
      const std::uint64_t hash = in.u64();
      const double clock = in.f64();
      const double day = in.f64();
      if (!in.ok || slot >= kSlots) return;
      my_slot_ = slot;
      data_mismatch_ = hash != data_.hash;
      if (!seeded_ || chunks_.seed() != seed) chunks_.reset(seed, "");
      seeded_ = true;
      clock_ = clock;
      day_len_ = day > 0.0 ? day : day_len_;
      break;
    }
    case kMsgChunk: {
      const std::uint32_t n = in.u32();
      if (!in.need(n) || !seeded_) return;
      std::vector<std::uint8_t> enc(bytes.begin() + static_cast<std::ptrdiff_t>(in.p),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(in.p + n));
      Chunk c;
      std::uint64_t seed = 0;
      if (ChunkMap::decode(enc, &c, &seed) && seed == chunks_.seed()) chunks_.put_chunk(c);
      break;
    }
    case kEdits: {
      const int n = in.u16();
      for (int i = 0; i < n && in.ok && seeded_; ++i) {
        const int tx = in.i32(), ty = in.i32();
        Tile t;
        t.ground = in.u8();
        t.object = in.u8();
        t.damage = in.u8();
        if (in.ok && t.ground < kGroundCount && t.object < kObjCount) {
          chunks_.set(tx, ty, t);
          ++edits_received_;
        }
      }
      chunks_.take_edits();
      break;
    }
    case kState: {
      const double clock = in.f64();
      for (Player& p : players_) {
        p.present = in.u8() != 0;
        p.hp = in.u16();
        p.max_hp = std::max<int>(1, in.u16());
        const int hb = in.u8();
        if (&p != &players_[static_cast<std::size_t>(my_slot_)]) p.hotbar = hb;
        p.hurt = in.u8() ? 0.2 : 0.0;
      }
      const int nm = in.u16();
      std::vector<Mob> mobs;
      for (int i = 0; i < nm && in.ok; ++i) {
        Mob m;
        m.id = in.u32();
        m.def = in.u16();
        m.x = in.f32();
        m.y = in.f32();
        m.hp = in.u16();
        const std::uint8_t f = in.u8();
        m.hurt = (f & 1) ? 0.2f : 0.0f;
        m.settler = (f & 2) != 0;
        m.state = (f & 4) ? 1 : 0;
        if (m.def < static_cast<int>(data_.creatures.size())) mobs.push_back(m);
      }
      const int ns = in.u16();
      std::vector<Shot> shots;
      for (int i = 0; i < ns && in.ok; ++i) {
        Shot s;
        s.x = in.f32();
        s.y = in.f32();
        s.vx = in.f32();
        s.vy = in.f32();
        shots.push_back(s);
      }
      if (!in.ok) return;
      clock_ = clock;
      mobs_ = std::move(mobs);
      shots_ = std::move(shots);
      break;
    }
    case kInventory: {
      const int slot = in.u8();
      const int hb = in.u8();
      if (!in.ok || slot >= kSlots) return;
      Inventory inv;
      for (int i = 0; i < Inventory::kSlots; ++i) {
        const int item = in.u16();
        const int n = in.u16();
        if (item != 0xFFFF && n > 0 && data_.item_def(item)) inv.at(i) = Stack{item, n};
      }
      if (!in.ok) return;
      Player& p = players_[static_cast<std::size_t>(slot)];
      p.inv = inv;
      p.hotbar = std::clamp(hb, 0, Inventory::kHotbar - 1);
      break;
    }
    default:
      break;
  }
}

void FrontierSim::client_frame(const World& w) {
  if (!mirror_ || !seeded_) return;
  std::vector<std::pair<float, float>> centres;
  for (int s = 0; s < kSlots; ++s) {
    const runtime::Actor* a = w.player(s);
    if (!a || a->hidden) continue;
    const Aabb b = runtime::collider_box(a->data);
    centres.push_back({(b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f});
  }
  chunks_.stream(centres);
  // Target tile for the HUD outline.
  if (const runtime::Actor* a = w.player(my_slot_)) {
    const Aabb b = runtime::collider_box(a->data);
    Player& p = players_[static_cast<std::size_t>(my_slot_)];
    front_tile(b, a->dir, &p.target_tx, &p.target_ty);
  }
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
