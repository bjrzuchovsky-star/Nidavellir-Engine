// --smoke-mmo: the MMO backbone end to end on 127.0.0.1.

#include "SmokeMmo.h"

#include "editor/ProjectInfo.h"
#include "editor/plugins/TownHallPanel.h"
#include "editor/screens/Editor2DScreen.h"
#include "mmo/MmoClient.h"
#include "mmo/MmoCrypto.h"
#include "mmo/MmoLoadTest.h"
#include "mmo/MmoProtocol.h"
#include "mmo/MmoServer.h"
#include "mmo/MmoStore.h"
#include "mmo/MmoWorld.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;
using mmo::MmoClient;
using mmo::MmoServer;

namespace {

int fail(const std::string& m) {
  std::cerr << "[smoke] mmo FAIL: " << m << "\n";
  return 1;
}

// Pump server + clients in real time until done() or the timeout.
bool pump(MmoServer& s, std::vector<MmoClient*> cs, double seconds, const std::function<bool()>& done = {}) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto last = t0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto now = clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    s.update(dt);
    for (MmoClient* c : cs) c->update(dt);
    if (done && done()) return true;
    if (std::chrono::duration<double>(now - t0).count() > seconds) return !done;
  }
}

const mmo::ClientEntity* find_named(const MmoClient& c, const std::string& name) {
  for (const auto& [id, e] : c.entities())
    if (e.name == name) return &e;
  return nullptr;
}

bool heard(MmoClient& c, const std::string& channel, const std::string& needle) {
  for (const auto& m : c.inbox())
    if (m.channel == channel && m.text.find(needle) != std::string::npos) return true;
  return false;
}

int crypto_checks() {
  const mmo::PasswordCost cost{256, 2};
  const std::string h1 = mmo::hash_password("hunter2", cost), h2 = mmo::hash_password("hunter2", cost);
  if (h1.rfind("argon2i$", 0) != 0) return fail("hash format: " + h1);
  if (h1 == h2) return fail("salts should differ");
  if (!mmo::verify_password("hunter2", h1) || mmo::verify_password("hunter3", h1)) return fail("verify");
  if (mmo::verify_password("hunter2", "argon2i$garbage")) return fail("verify malformed");
  const std::string t = mmo::new_token();
  if (t.size() != 64 || t == mmo::new_token()) return fail("token");
  if (mmo::token_digest(t).size() != 64 || mmo::token_digest(t) == t) return fail("token digest");
  std::cout << "[smoke] mmo crypto OK (Argon2i salted hashes, tokens)\n";
  return 0;
}

int store_checks(const fs::path& dir) {
  const std::string path = (dir / "store.db").string();
  std::string err;
  std::int64_t acc = 0;
  {
    mmo::Store st;
    if (!st.open(path, &err)) return fail("store open: " + err);
    if (!st.create_account("Wren", mmo::hash_password("pw", {256, 1}), false, &acc)) return fail("account");
    std::int64_t dup = 0;
    if (st.create_account("wren", "x", false, &dup)) return fail("usernames are case-insensitive");
    mmo::CharacterRow row;
    row.account_id = acc;
    row.name = "Wren";
    row.zone = "town";
    row.x = 10;
    row.y = 20;
    if (!st.create_character(&row)) return fail("character");
    row.inventory["gold_nugget"] = 3;
    row.x = 99;
    if (!st.save_characters({row})) return fail("save");
    if (!st.put_session(mmo::token_digest("tok"), row.id, mmo::unix_now() + 60)) return fail("session");
  }
  mmo::Store st;
  if (!st.open(path, &err)) return fail("reopen: " + err);
  const auto row = st.character_of(acc);
  if (!row || row->x != 99 || row->inventory.count("gold_nugget") == 0 || row->inventory.at("gold_nugget") != 3)
    return fail("store should keep the character and inventory");
  if (st.session_character(mmo::token_digest("tok")) != row->id) return fail("session lookup");
  if (st.schema_version() != 1) return fail("schema version");
  std::cout << "[smoke] mmo store OK (SQLite accounts, characters, inventory, sessions)\n";
  return 0;
}

int protocol_checks() {
  mmo::LoginMsg m;
  m.user = "wren";
  m.password = "pw";
  m.character = "Wren";
  mmo::LoginMsg back;
  const auto bytes = mmo::encode(m);
  if (!mmo::decode(bytes, &back) || back.user != "wren" || back.character != "Wren") return fail("login round trip");
  for (std::size_t n = 0; n + 1 < bytes.size(); ++n) {
    std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<long>(n));
    if (mmo::decode(cut, &back)) return fail("truncated message should not decode");
  }
  mmo::MmoMsg kind;
  if (mmo::peek({0xff, 0x00}, &kind)) return fail("unknown type should not peek");
  std::cout << "[smoke] mmo protocol OK\n";
  return 0;
}

int world_checks(const fs::path& dir) {
  std::string err;
  if (!mmo::write_sample_world(dir.string(), &err)) return fail(err);
  mmo::WorldConfig cfg;
  if (!mmo::load_world_config(dir.string(), &cfg, &err)) return fail("world config: " + err);
  if (cfg.zones.size() != 2 || !cfg.zone("town") || !cfg.zone("mine")) return fail("sample world zones");
  const mmo::ZoneDef* town = cfg.zone("town");
  float x = town->spawn_x, y = town->spawn_y;
  for (int i = 0; i < 400; ++i) mmo::step_rider(town->geo, cfg.speed, 0.05f, -1.0f, 0.0f, &x, &y);
  if (x < town->geo.bounds.x0 || x > town->geo.bounds.x1) return fail("riders stay in bounds");
  std::cout << "[smoke] mmo world OK (mmo.json zones, movement)\n";
  return 0;
}

int live_checks(const fs::path& proj) {
  const std::string db = (proj / "mmo" / "smoke.db").string();
  auto start = [&](MmoServer& s) {
    mmo::MmoServerConfig cfg;
    cfg.project_dir = proj.string();
    cfg.db_path = db;
    cfg.port = 0;
    cfg.argon2_kib = 256;
    cfg.save_every = 1.0;
    std::string err;
    if (!s.start(std::move(cfg), &err)) std::cerr << "[smoke] mmo server: " << err << "\n";
    return s.running();
  };
  const std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string old_token;
  {
    MmoServer s;
    if (!start(s)) return fail("server start");
    port = s.port();
    std::string err;
    if (!s.create_account("sheriff", "tin-star", true, "Sheriff", &err)) return fail("admin account: " + err);
    MmoClient a, b, bad;
    a.create(host, port, "alice", "pw-alice", "Alice");
    b.create(host, port, "bob", "pw-bob", "Bob");
    if (!pump(s, {&a, &b}, 8, [&] { return a.in_world() && b.in_world(); }))
      return fail("logins: " + a.error() + " / " + b.error());
    bad.set_auto_resume(false);
    bad.login(host, port, "alice", "wrong");
    if (!pump(s, {&a, &b, &bad}, 5, [&] { return bad.state() == MmoClient::State::Failed; }))
      return fail("a wrong password should fail");
    MmoClient dup;
    dup.create(host, port, "carol", "pw-carol", "alice");
    if (!pump(s, {&a, &b, &dup}, 5, [&] { return dup.state() == MmoClient::State::Failed; }))
      return fail("a taken rider name should fail");
    std::cout << "[smoke] mmo logins OK (create, wrong password, name taken)\n";

    // Interest: neighbours appear, a far rider vanishes.
    if (!pump(s, {&a, &b}, 4, [&] { return find_named(a, "Bob") != nullptr; })) return fail("Alice should see Bob");
    s.admin("tp Bob 3600 3600");
    if (!pump(s, {&a, &b}, 4, [&] { return find_named(a, "Bob") == nullptr; }))
      return fail("Bob should vanish from Alice's view");
    std::cout << "[smoke] mmo interest OK (appear / vanish by radius)\n";

    // Movement: honest prediction never needs a correction.
    const float ay = a.y();
    a.set_move(0.0f, -1.0f);
    pump(s, {&a, &b}, 1.0);
    a.set_move(0.0f, 0.0f);
    pump(s, {&a, &b}, 0.4);
    if (a.y() > ay - 60.0f) return fail("Alice should have ridden north");
    if (a.stats().corrections != 0) return fail("honest movement was corrected");
    std::cout << "[smoke] mmo movement OK (predicted, 0 corrections)\n";

    // Chat: world, whisper, rate limit.
    a.say("world", "howdy from Alice");
    a.say("say", "/w Bob psst");
    if (!pump(s, {&a, &b}, 3, [&] { return heard(b, "world", "howdy") && heard(b, "whisper", "psst"); }))
      return fail("Bob should hear world chat and the whisper");
    s.admin("mute Bob 1");
    b.say("world", "can you hear me");
    pump(s, {&a, &b}, 0.6);
    if (heard(a, "world", "can you hear me")) return fail("muted riders should not be heard");
    s.admin("unmute Bob");
    std::cout << "[smoke] mmo chat OK (world, whisper, mute)\n";

    // Pickup: walk back to spawn and take the nearest nugget.
    s.admin("tp Alice 2050 2000");
    pump(s, {&a, &b}, 1.0);
    std::uint64_t nugget = 0;
    for (const auto& [id, e] : a.entities())
      if (e.kind == mmo::EntityKind::Item && std::fabs(e.x - 2060) < 5 && std::fabs(e.y - 2000) < 5) nugget = id;
    if (!nugget) return fail("Alice should see the nugget");
    a.interact(nugget);
    if (!pump(s, {&a, &b}, 3, [&] { return a.inventory().count("gold_nugget") > 0; })) return fail("pickup");
    std::cout << "[smoke] mmo pickup OK\n";

    // Anti-cheat: a rider claiming impossible positions is corrected, then kicked.
    MmoClient cheat;
    cheat.set_auto_resume(false);
    cheat.create(host, port, "mallory", "pw-mallory", "Mallory");
    if (!pump(s, {&a, &b, &cheat}, 5, [&] { return cheat.in_world(); })) return fail("cheater login");
    cheat.cheat_offset(300.0f, 0.0f);
    cheat.set_move(1.0f, 0.0f);
    if (!pump(s, {&a, &b, &cheat}, 8, [&] { return !cheat.in_world(); })) return fail("teleporter should be kicked");
    if (cheat.disconnect_reason() != mmo::kMmoCheat) return fail("kick reason should be cheat");
    std::cout << "[smoke] mmo anti-cheat OK (teleports corrected, then kicked)\n";

    // Resume: a dropped line comes back as the same rider.
    old_token = a.token();
    a.drop_line();
    if (!pump(s, {&a, &b}, 8, [&] { return a.in_world() && a.stats().resumes >= 1; })) return fail("resume");
    if (a.token() == old_token) return fail("resume should hand out a new token");
    MmoClient stale;
    stale.set_auto_resume(false);
    stale.resume(host, port, old_token);
    if (!pump(s, {&a, &b, &stale}, 5, [&] { return stale.state() == MmoClient::State::Failed; }))
      return fail("an old token should be refused");
    std::cout << "[smoke] mmo resume OK (new token, old token refused)\n";

    // Admin: kick, stats, Strongbox.
    if (s.admin("stats").empty() || s.admin("who").find("Alice") == std::string::npos) return fail("admin who/stats");
    s.admin("kick Bob");
    if (!pump(s, {&a, &b}, 4, [&] { return !b.in_world(); })) return fail("admin kick");
    if (!s.export_to_strongbox("Alice", "alice", &err)) return fail("strongbox export: " + err);
    if (!fs::exists(proj / "saves" / "slots" / "alice.save.json")) return fail("strongbox slot file");
    std::cout << "[smoke] mmo admin OK (who, stats, mute, kick, Strongbox export)\n";
    a.quit();
    pump(s, {&a}, 0.5);
    s.stop();
  }
  // Persistence: a fresh server reads the same database.
  {
    MmoServer s;
    if (!start(s)) return fail("restart");
    port = s.port();
    MmoClient a;
    a.login(host, port, "alice", "pw-alice");
    if (!pump(s, {&a}, 8, [&] { return a.in_world() && !a.inventory().empty(); }))
      return fail("Alice should come back with her nugget: " + a.error());
    std::string err;
    if (!s.import_from_strongbox("Alice", "alice", &err)) return fail("strongbox import: " + err);
    a.quit();
    pump(s, {&a}, 0.5);
    s.stop();
  }
  std::cout << "[smoke] mmo persistence OK (restart keeps riders and inventory; Strongbox import)\n";
  return 0;
}

int load_checks(const fs::path& dir) {
  mmo::LoadTestOptions opt;
  opt.bots = 48;
  opt.seconds = 4.0;
  opt.project_dir = (dir / "load").string();
  opt.db_path = (dir / "load" / "load.db").string();
  const mmo::LoadTestResult r = mmo::run_load_test(opt);
  if (!r.ok) return fail("load test: " + r.failure);
  if (r.in_world < 40) return fail("load test: only " + std::to_string(r.in_world) + " bots in the world");
  if (r.cheat_kicks != 0) return fail("load test: honest bots were kicked");
  if (r.world_entities > 0 && r.avg_visible >= r.world_entities) return fail("load test: interest did not filter");
  std::cout << "[smoke] mmo load test OK: " << r.summary << "\n";
  return 0;
}

int panel_checks(const fs::path& dir) {
  fs::create_directories(dir / "p2d");
  std::string err;
  if (!mmo::write_sample_world((dir / "p2d").string(), &err)) return fail(err);
  editor::ProjectInfo info;
  info.id = "_mmo_editor";
  info.name = "Town Hall";
  info.path = (dir / "p2d").string();
  info.plugins = {"ts.posse"};
  info.plugins_set = true;
  editor::Editor2DScreen scr(info);
  scr.on_enter();
  bool found = false;
  for (const auto& pp : scr.plugins().manager().editor_panels()) found |= pp.item.id == "posse.town_hall";
  scr.on_exit();
  if (!found) return fail("editor: the Town Hall panel should register with ts.posse");

  editor::TownHallState th;
  th.project_dir = (dir / "p2d").string();
  th.port = 0;
  th.db_path = (dir / "p2d" / "mmo" / "panel.db").string();
  if (!editor::town_hall_open(th, &err)) return fail("Town Hall open: " + err);
  th.host.set_bots(6);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* c = ImGui::CreateContext();
  ImGui::SetCurrentContext(c);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280, 800);
  io.IniFilename = nullptr;
  unsigned char* px = nullptr;
  int fw = 0, fh = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  for (int frame = 0; frame < 3; ++frame) {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("Town Hall");
    editor::draw_town_hall_panel(th);
    ImGui::End();
    ImGui::Render();
  }
  ImGui::DestroyContext(c);
  ImGui::SetCurrentContext(prev);
  int riders = 0;
  th.host.with([&](MmoServer& s, mmo::BotSwarm*) { riders = static_cast<int>(s.characters().size()); });
  const std::string who = th.host.admin("who");
  editor::town_hall_close(th);
  if (riders < 1 || who.empty()) return fail("Town Hall: bots should ride on the editor's server");
  std::cout << "[smoke] mmo editor OK (Town Hall panel, in-editor server, " << riders << " bots)\n";
  return 0;
}

}  // namespace

int run_mmo_smoke() {
  const fs::path root = fs::absolute("./TombStoneProjects/_mmo_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (crypto_checks() != 0) return 1;
  if (store_checks(root) != 0) return 1;
  if (protocol_checks() != 0) return 1;
  if (world_checks(root / "world") != 0) return 1;
  if (live_checks(root / "world") != 0) return 1;
  if (load_checks(root) != 0) return 1;
  if (panel_checks(root / "editor") != 0) return 1;
  std::cout << "[smoke] mmo OK\n";
  return 0;
}
