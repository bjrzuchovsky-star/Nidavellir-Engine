// --smoke-team: Live Team Claims stage 1 (docs/team-claims.md).
#include "SmokeTeam.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/team/TeamSession.h"
#include "net/Net.h"
#include "net/SimTransport.h"
#include "render3d/Scene3D.h"
#include "scene/SceneJson.h"
#include "team/TeamClient.h"
#include "team/TeamServer.h"
#include "vault/Sha256.h"
#include "vault/VaultServer.h"
#include "vault/VaultStore.h"

#include <fstream>
#include <sstream>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace tc = ts::tombstone::team;
namespace net = ts::tombstone::net;
using ts::tombstone::Entity2D;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] team FAILED: " << what << '\n';
  return 1;
}

ed::ProjectInfo fresh(const std::string& id, ed::ProjectKind kind) {
  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ed::ProjectInfo info;
  info.id = id;
  info.name = id;
  info.kind = kind;
  info.path = dir.string();
  return info;
}

using Tick = std::function<void(float)>;

// Run every participant until cond() holds (or the time is up).
bool pump(const std::vector<Tick>& ticks, const std::function<bool()>& cond, double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    for (const Tick& t : ticks) t(0.016f);
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  for (const Tick& t : ticks) t(0.016f);
  return cond();
}

void settle(const std::vector<Tick>& ticks, double seconds) {
  pump(ticks, [] { return false; }, seconds);
}

std::string doc_of(ed::TeamSession& t) { return t.binding().capture().canonical(); }

std::function<std::unique_ptr<net::Transport>(std::unique_ptr<net::Transport>)> laggy(int ms) {
  return [ms](std::unique_ptr<net::Transport> inner) -> std::unique_ptr<net::Transport> {
    net::LinkSim sim;
    sim.latency_ms = ms;
    sim.jitter_ms = ms / 4;
    return std::make_unique<net::SimTransport>(std::move(inner), sim);
  };
}

// ---- 2D: a ts_vault-style server (API keys) + two editors + a late joiner ----

int check_2d() {
  tc::TeamServer server;
  tc::ServerOptions so;
  so.port = 0;
  so.lock_timeout_s = 1.5;  // a crashed editor's locks go quickly in the smoke
  so.member_timeout_s = 3.0;
  so.compact_every = 4;     // late joiners need snapshot + tail
  so.engine = "0.2.0-smoke";
  so.check_key = [](const std::string& key, std::string* name, bool* can_write) {
    if (key == "tsv_annie") *name = "Annie", *can_write = true;
    else if (key == "tsv_wyatt") *name = "Wyatt", *can_write = true;
    else if (key == "tsv_late") *name = "Doc", *can_write = true;
    else if (key == "tsv_watch") *name = "Watcher", *can_write = false;
    else return false;
    return true;
  };
  std::string err;
  if (!server.start(so, &err)) return fail("server start: " + err);
  const std::uint16_t port = server.port();

  ed::Editor2DScreen a(fresh("_team_smoke_a", ed::ProjectKind::TwoD));
  ed::Editor2DScreen b(fresh("_team_smoke_b", ed::ProjectKind::TwoD));
  a.on_enter();
  b.on_enter();
  a.workspace().set_snap_enabled(false);
  b.workspace().set_snap_enabled(false);
  a.workspace().set_object_snap(false);
  b.workspace().set_object_snap(false);
  // B starts with a different scene: joining swaps it for the posse's.
  b.create_entity("Only On B");

  std::vector<Tick> ticks = {[&](float) { server.poll(); }, [&](float dt) { a.tick_team(dt); },
                             [&](float dt) { b.tick_team(dt); }};

  ed::TeamJoinSettings ja;
  ja.host = "127.0.0.1";
  ja.port = port;
  ja.key = "tsv_annie";
  ja.claim = "_team_claim";  // names the claim -> seeds it from A's scene
  ja.wrap_transport = laggy(20);
  if (!a.team().join(ja, &err)) return fail("A join: " + err);
  if (!pump(ticks, [&] { return a.team().joined(); }, 5)) return fail("A joined: " + a.team().client().error());
  ed::TeamJoinSettings jb = ja;
  jb.key = "tsv_wyatt";
  jb.claim = "*";
  jb.wrap_transport = laggy(35);
  if (!b.team().join(jb, &err)) return fail("B join: " + err);
  if (!pump(ticks, [&] { return b.team().joined() && a.team().client().members().size() == 1; }, 5)) {
    return fail("B joined: " + b.team().client().error());
  }
  settle(ticks, 0.3);
  if (doc_of(a.team()) != doc_of(b.team())) return fail("B got A's scene on join");
  for (const Entity2D& e : b.workspace().entities()) {
    if (e.name == "Only On B") return fail("B's own scene was replaced");
  }
  if (a.team().client().my_name() != "Annie" || b.team().client().my_name() != "Wyatt") {
    return fail("names from the API keys");
  }
  if (a.team().client().my_colour() == b.team().client().my_colour()) return fail("distinct colours");

  // Ids: both create at once, ids come from separate grants.
  const std::uint64_t ea = a.create_entity("Annie's Crate");
  const std::uint64_t eb = b.create_entity("Wyatt's Barrel");
  if (ea == eb) return fail("ids collide (" + std::to_string(ea) + ")");
  if (ea < a.team().client().id_lo() || ea >= a.team().client().id_hi()) return fail("A id outside grant");
  if (eb < b.team().client().id_lo() || eb >= b.team().client().id_hi()) return fail("B id outside grant");
  auto converged = [&] {
    const tc::Docs* d = server.docs("_team_claim");
    if (!d || !d->count("scene.json")) return false;
    const std::string truth = d->at("scene.json").canonical();
    return a.team().client().idle() && b.team().client().idle() && doc_of(a.team()) == truth &&
           doc_of(b.team()) == truth;
  };
  if (!pump(ticks, converged, 5)) return fail("converge after concurrent creates");
  if (!b.workspace().find(ea) || !a.workspace().find(eb)) return fail("both creates on both sides");

  // Conflicting edits to one unlocked entity in the same frame: the server
  // order decides and everyone ends up with the same answer.
  a.workspace().clear_selection();
  b.workspace().clear_selection();
  settle(ticks, 0.2);
  a.workspace().find(ea)->x = 111.0f;
  b.workspace().find(ea)->x = 222.0f;
  if (!pump(ticks, converged, 5)) return fail("converge after a conflicting edit");
  const float x_final = a.workspace().find(ea)->x;
  if (x_final != 111.0f && x_final != 222.0f) return fail("conflict result is one of the edits");

  // Soft locks: A selects the crate; B sees the lock and B's drag is turned back.
  a.workspace().select(ea);
  if (!pump(ticks, [&] { return b.team().lock_holder(ea) == a.team().client().me(); }, 5)) {
    return fail("B sees A's lock");
  }
  if (a.team().lock_holder(ea) != 0) return fail("own lock is not 'someone else'");
  const std::uint64_t rejects0 = server.rejects();
  b.workspace().select(ea);  // B's lock request is denied (A holds it)
  b.begin_drag_move();
  b.workspace().update_move(500.0f, 0.0f);
  b.end_drag_move();
  if (b.workspace().find(ea)->x != x_final + 500.0f) return fail("B's drag applied optimistically");
  if (!pump(ticks, [&] { return server.rejects() > rejects0 && converged(); }, 5)) {
    return fail("locked edit rejected + rolled back");
  }
  if (b.workspace().find(ea)->x != x_final) return fail("B's locked drag undone");
  if (b.team().client().rejected() == 0) return fail("B counted the reject");
  // ... while A's own drag on it goes through.
  a.begin_drag_move();
  a.workspace().update_move(10.0f, 5.0f);
  a.end_drag_move();
  if (!pump(ticks, converged, 5)) return fail("A's drag on its locked entity");
  if (b.workspace().find(ea)->x != x_final + 10.0f) return fail("A's drag reached B");

  // Undo is personal: A undoes its drag; B's barrel stays put.
  b.workspace().clear_selection();
  b.workspace().select(eb);
  b.begin_drag_move();
  b.workspace().update_move(0.0f, 40.0f);
  b.end_drag_move();
  if (!pump(ticks, converged, 5)) return fail("B's barrel drag");
  const float barrel_y = a.workspace().find(eb)->y;
  if (!a.undo()) return fail("A undo");
  if (!pump(ticks, converged, 5)) return fail("converge after A's undo");
  if (b.workspace().find(ea)->x != x_final) return fail("A's undo reached B");
  if (a.workspace().find(eb)->y != barrel_y) return fail("A's undo kept B's barrel move");

  // Inspector-style edit (coalesced) and a delete, then the order.
  a.begin_inspector_edit("Name");
  a.workspace().find(ea)->name = "Annie's Big Crate";
  a.end_inspector_edit();
  b.workspace().clear_selection();
  const std::uint64_t doomed = b.create_entity("Tumbleweed");
  if (!pump(ticks, converged, 5)) return fail("inspector + create");
  b.workspace().select(doomed);
  settle(ticks, 0.2);
  b.delete_selected();
  if (!pump(ticks, converged, 5)) return fail("delete");
  if (a.workspace().find(doomed)) return fail("delete reached A");
  if (b.workspace().find(ea)->name != "Annie's Big Crate") return fail("inspector edit reached B");

  // Presence: cursors and selection travel.
  a.team().set_cursor(64.0f, 32.0f, 0.0f, true);
  if (!pump(ticks, [&] {
        const tc::RemoteMember* m = b.team().client().member(a.team().client().me());
        float xyz[3];
        return m && m->has_presence && m->presence.floats("cursor", xyz, 3) && xyz[0] == 64.0f &&
               m->presence["sel"].size() == 1;
      }, 5)) {
    return fail("A's cursor + selection reached B");
  }

  // Late join: C gets the snapshot + op tail (compact_every 4 -> both used).
  ed::Editor2DScreen c(fresh("_team_smoke_c", ed::ProjectKind::TwoD));
  c.on_enter();
  ticks.push_back([&](float dt) { c.tick_team(dt); });
  ed::TeamJoinSettings jc = jb;
  jc.key = "tsv_late";
  jc.wrap_transport = nullptr;
  if (!c.team().join(jc, &err)) return fail("C join: " + err);
  if (!pump(ticks, [&] { return c.team().joined(); }, 5)) return fail("C joined");
  settle(ticks, 0.2);
  const tc::SessionInfo si = server.sessions().at(0);
  if (si.snap_seq == 0) return fail("op log compacted into a snapshot");
  if (doc_of(c.team()) != server.docs("_team_claim")->at("scene.json").canonical()) {
    return fail("late joiner matches the server");
  }
  std::cout << "[smoke] team: late join at seq " << c.team().client().seq() << " (snapshot "
            << si.snap_seq << " + tail " << (si.seq - si.snap_seq) << ")\n";

  // Read-only key: edits are put back.
  ed::Editor2DScreen v(fresh("_team_smoke_v", ed::ProjectKind::TwoD));
  v.on_enter();
  ticks.push_back([&](float dt) { v.tick_team(dt); });
  ed::TeamJoinSettings jv = jc;
  jv.key = "tsv_watch";
  if (!v.team().join(jv, &err) || !pump(ticks, [&] { return v.team().joined(); }, 5)) {
    return fail("viewer join");
  }
  settle(ticks, 0.2);
  const std::uint64_t commits0 = server.commits();
  v.create_entity("Graffiti");
  settle(ticks, 1.5);
  if (server.commits() != commits0) return fail("viewer edit reached the server");
  if (doc_of(v.team()) != server.docs("_team_claim")->at("scene.json").canonical()) {
    return fail("viewer edit put back");
  }
  v.team().leave();

  // Bad key and version guard.
  {
    tc::TeamClient bad;
    tc::JoinOptions o;
    o.port = port;
    o.claim = "*";
    o.key = "tsv_nope";
    o.scene2d = ts::tombstone::scene_json::kSceneVersion;
    o.scene3d = ts::tombstone::scene3d::kScene3DVersion;
    o.dim = "2d";
    bad.join(o, &err);
    ticks.push_back([&](float) { bad.poll(); });
    if (!pump(ticks, [&] { return bad.state() == tc::TeamClient::State::Refused; }, 5)) {
      return fail("bad key refused");
    }
    ticks.pop_back();
    tc::TeamClient old;
    o.key = "tsv_late";
    o.scene2d = ts::tombstone::scene_json::kSceneVersion - 1;
    old.join(o, &err);
    ticks.push_back([&](float) { old.poll(); });
    if (!pump(ticks, [&] { return old.state() == tc::TeamClient::State::Refused; }, 5)) {
      return fail("older scene format refused");
    }
    if (old.error().find("Update TombStone") == std::string::npos) return fail("refusal says why");
    ticks.pop_back();
    // Another engine build with the same scene formats rides, with a warning.
    tc::TeamClient newer;
    o.scene2d = ts::tombstone::scene_json::kSceneVersion;
    o.engine = "9.9.9";
    newer.join(o, &err);
    ticks.push_back([&](float) { newer.poll(); });
    if (!pump(ticks, [&] { return newer.joined(); }, 5)) return fail("other engine, same formats joins");
    if (newer.warning().find("Engine versions differ") == std::string::npos) {
      return fail("engine version mismatch warns");
    }
    newer.leave();
    ticks.pop_back();
  }

  // Disconnect: A (holding the crate's lock) crashes; its lock is released.
  a.workspace().select(ea);
  if (!pump(ticks, [&] { return b.team().lock_holder(ea) == a.team().client().me(); }, 5)) {
    return fail("A holds the crate again");
  }
  const std::uint32_t a_id = a.team().client().me();
  a.team().drop_now();
  if (!pump(ticks, [&] { return b.team().lock_holder(ea) == 0; }, 8)) return fail("crashed A's lock released");
  if (!pump(ticks, [&] { return !b.team().client().member(a_id); }, 8)) return fail("crashed A left the posse");
  // Now B may edit the crate.
  b.workspace().select(ea);
  settle(ticks, 0.3);
  b.begin_drag_move();
  b.workspace().update_move(-30.0f, 0.0f);
  b.end_drag_move();
  auto bc_converged = [&] {
    const std::string truth = server.docs("_team_claim")->at("scene.json").canonical();
    return b.team().client().idle() && doc_of(b.team()) == truth && doc_of(c.team()) == truth;
  };
  if (!pump(ticks, bc_converged, 5)) return fail("B edits the released crate");
  if (c.workspace().find(ea)->x != x_final - 30.0f) return fail("B's edit reached C");

  // Graceful leave releases locks at once.
  const std::uint32_t b_id = b.team().client().me();
  b.team().leave();
  if (!pump(ticks, [&] { return c.team().lock_holder(ea) == 0 && !c.team().client().member(b_id); }, 3)) {
    return fail("B's leave releases its locks");
  }
  const std::uint64_t resyncs = a.team().resyncs() + b.team().resyncs() + c.team().resyncs();
  std::cout << "[smoke] team 2D: resyncs A " << a.team().resyncs() << " B " << b.team().resyncs()
            << " C " << c.team().resyncs() << '\n';
  std::cout << "[smoke] team 2D: OK (" << server.commits() << " commits, " << server.rejects()
            << " rejected, " << resyncs << " resyncs)\n";
  c.team().leave();
  server.stop();
  return 0;
}

// ---- Assets: files travel through a Vault by SHA-256 ----

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int check_assets() {
  namespace tv = ts::tombstone::vault;
  const fs::path vroot = fs::temp_directory_path() / "ts_team_smoke_vault";
  std::error_code ec;
  fs::remove_all(vroot, ec);
  tv::VaultStore store;
  std::string err;
  if (!store.open(vroot.string(), &err)) return fail("vault open: " + err);
  const std::string key = store.add_key("annie", true, &err);
  tv::VaultServer vs(store);
  tv::ServerOptions vo;
  vo.port = 0;
  if (key.empty() || !vs.start(vo, &err)) return fail("vault start: " + err);
  const std::string url = "http://127.0.0.1:" + std::to_string(vs.port());
  int rc = 0;
  {
    tc::TeamServer server;
    tc::ServerOptions so;
    so.port = 0;
    so.check_key = [&store](const std::string& k, std::string* name, bool* can_write) {
      ts::tombstone::vault::KeyInfo info;
      if (!store.check_key(k, &info)) return false;
      *name = info.name;
      *can_write = info.can_write;
      return true;
    };
    if (!server.start(so, &err)) return fail("asset team server: " + err);
    ed::Editor2DScreen a(fresh("_team_assets_a", ed::ProjectKind::TwoD));
    ed::Editor2DScreen b(fresh("_team_assets_b", ed::ProjectKind::TwoD));
    a.on_enter();
    b.on_enter();
    const fs::path pa(a.team().project().path), pb(b.team().project().path);
    fs::create_directories(pa / "assets", ec);
    std::ofstream(pa / "assets" / "wanted.txt", std::ios::binary) << "WANTED: one ghost, dead or alive";
    std::vector<Tick> ticks = {[&](float) { server.poll(); }, [&](float dt) { a.tick_team(dt); },
                               [&](float dt) { b.tick_team(dt); }};
    ed::TeamJoinSettings j;
    j.port = server.port();
    j.key = key;
    j.vault_url = url;
    j.claim = "_team_assets";
    j.user = "Annie";
    if (!a.team().join(j, &err) || !pump(ticks, [&] { return a.team().joined(); }, 5)) return fail("asset A join");
    j.claim = "*";
    j.user = "Wyatt";
    if (!b.team().join(j, &err) || !pump(ticks, [&] { return b.team().joined(); }, 5)) return fail("asset B join");
    const fs::path got = pb / "assets" / "wanted.txt";
    if (!pump(ticks, [&] { return fs::exists(got) && slurp(got) == slurp(pa / "assets" / "wanted.txt"); }, 10)) {
      rc = fail("asset reached B through the Vault");
    }
    const std::string sha = tv::sha256_file((pa / "assets" / "wanted.txt").string());
    if (!rc && !fs::exists(vroot / "blobs" / sha.substr(0, 2) / sha)) rc = fail("blob stored by SHA-256");
    // B changes it; A gets the new version.
    if (!rc) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      std::ofstream(got, std::ios::binary | std::ios::trunc) << "REWARD: 500 dollars";
      if (!pump(ticks, [&] { return slurp(pa / "assets" / "wanted.txt") == "REWARD: 500 dollars"; }, 10)) {
        rc = fail("changed asset reached A");
      }
    }
    if (!rc) {
      std::cout << "[smoke] team assets: OK (A sent " << a.team().assets_sent() << ", B fetched "
                << b.team().assets_fetched() << ", A fetched " << a.team().assets_fetched() << ")\n";
    }
    b.team().leave();
    a.team().leave();
    server.stop();
  }
  vs.stop();
  fs::remove_all(vroot, ec);
  return rc;
}

// ---- 3D: Host from my editor + a joiner ----

int check_3d() {
  ed::Editor3DScreen a(fresh("_team_smoke_3a", ed::ProjectKind::ThreeD));
  ed::Editor3DScreen b(fresh("_team_smoke_3b", ed::ProjectKind::ThreeD));
  a.on_enter();
  b.on_enter();
  std::string err;
  ed::TeamHostSettings hs;
  hs.port = 0;  // any free port
  hs.pass = "dusk";
  hs.user = "Calamity";
  if (!a.team().host(hs, &err)) return fail("3D host: " + err);
  std::vector<Tick> ticks = {[&](float dt) { a.tick_team(dt); }, [&](float dt) { b.tick_team(dt); }};
  if (!pump(ticks, [&] { return a.team().joined(); }, 5)) return fail("host joined itself");
  ed::TeamJoinSettings j;
  j.port = a.team().server()->port();
  j.pass = "wrong";
  j.user = "Hickok";
  b.team().join(j, &err);
  if (!pump(ticks, [&] { return b.team().client().state() == tc::TeamClient::State::Refused; }, 5)) {
    return fail("wrong passcode refused");
  }
  j.pass = "dusk";
  if (!b.team().join(j, &err) || !pump(ticks, [&] { return b.team().joined(); }, 5)) {
    return fail("3D join: " + b.team().client().error());
  }
  settle(ticks, 0.3);
  if (doc_of(a.team()) != doc_of(b.team())) return fail("3D joiner has the host's scene");
  auto converged = [&] {
    const std::string truth = a.team().server()->docs(a.project().id)->at("scene.json").canonical();
    return a.team().client().idle() && b.team().client().idle() && doc_of(a.team()) == truth &&
           doc_of(b.team()) == truth;
  };
  const std::uint64_t ca = a.create_primitive("cube", {1, 0.5f, 0});
  const std::uint64_t cb = b.create_primitive("sphere", {-1, 0.5f, 0});
  if (!ca || !cb || ca == cb) return fail("3D concurrent creates get distinct ids");
  if (!pump(ticks, converged, 5)) return fail("3D converge after creates");
  ts::tombstone::render3d::Transform t = b.find(ca)->transform;
  a.clear_selection();
  t.position.x = 7.5f;
  b.set_transform(ca, t, "Move");
  if (!pump(ticks, converged, 5)) return fail("3D move");
  if (a.find(ca)->transform.position.x != 7.5f) return fail("3D move reached the host");
  ts::tombstone::render3d::Environment env = a.scene().environment;
  env.ambient = 0.123f;
  a.set_environment(env, "Ambient");
  if (!pump(ticks, converged, 5)) return fail("3D environment");
  if (b.scene().environment.ambient != 0.123f) return fail("3D environment reached B");
  // Locks in 3D: host selects the sphere, B's move on it is turned back.
  b.clear_selection();  // B's create selected (and locked) the sphere
  settle(ticks, 0.2);
  a.select(cb);
  if (!pump(ticks, [&] { return b.team().lock_holder(cb) != 0; }, 5)) return fail("3D lock seen");
  t = b.find(cb)->transform;
  const float before = t.position.y;
  t.position.y = 9.0f;
  b.set_transform(cb, t, "Lift");
  if (!pump(ticks, [&] { return converged() && b.find(cb)->transform.position.y == before; }, 5)) {
    return fail("3D locked edit rolled back");
  }
  // Host leaves: the session ends for B.
  a.team().leave();
  if (!pump(ticks, [&] { return !b.team().joined(); }, 12)) return fail("host leaving ends the session");
  std::cout << "[smoke] team 3D (editor host): OK\n";
  return 0;
}

}  // namespace

namespace ts {
namespace tombstone {

int run_team_smoke() {
  if (!net::net_startup()) return fail("net startup");
  int rc = check_2d();
  if (rc == 0) rc = check_assets();
  if (rc == 0) rc = check_3d();
  net::net_shutdown();
  if (rc == 0) std::cout << "[smoke] team (Live Team Claims): OK\n";
  return rc;
}

}  // namespace tombstone
}  // namespace ts
