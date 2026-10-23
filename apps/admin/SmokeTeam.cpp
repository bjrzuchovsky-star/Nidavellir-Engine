// --smoke-team: Live Team Claims stage 1 (docs/team-claims.md).
#include "SmokeTeam.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/team/TeamSession.h"
#include "net/Net.h"
#include "net/SimTransport.h"
#include "render3d/Scene3D.h"
#include "terrain/TerrainView.h"
#include "scene/SceneJson.h"
#include "team/TeamClient.h"
#include "team/TeamServer.h"
#include "vault/Sha256.h"
#include "vault/VaultBackend.h"
#include "vault/VaultServer.h"
#include "vault/VaultStore.h"

#include <fstream>
#include <sstream>

#include <algorithm>
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
  so.lock_idle_s = 1.0;     // take-over after 1 s idle (15 s by default)
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

  // Mid-drag preview: B sees A's crate move while A drags; nothing is
  // committed until A lets go. (Y only: later checks use the crate's x.)
  {
    auto truth = [&] { return server.docs("_team_claim")->at("scene.json").canonical(); };
    if (!pump(ticks, [&] { return b.team().lock_holder(ea) == a.team().client().me(); }, 5)) {
      return fail("A still holds the crate");
    }
    const float y0 = a.workspace().find(ea)->y;
    const std::string before = truth();
    const std::uint64_t commits0 = server.commits();
    a.begin_drag_move();
    a.workspace().update_move(0.0f, 25.0f);
    if (!pump(ticks, [&] { return b.workspace().find(ea)->y == y0 + 25.0f; }, 5)) {
      return fail("B sees A's drag mid-way");
    }
    if (!b.team().previewing(ea)) return fail("B marks the crate as previewed");
    a.workspace().update_move(0.0f, 60.0f);
    if (!pump(ticks, [&] { return b.workspace().find(ea)->y == y0 + 60.0f; }, 5)) {
      return fail("B follows the drag");
    }
    settle(ticks, 0.2);
    if (server.commits() != commits0 || truth() != before) return fail("a preview commits nothing");
    a.end_drag_move();
    if (!pump(ticks, [&] { return converged() && !b.team().previewing(ea); }, 5)) {
      return fail("drag released -> committed once");
    }
    if (b.workspace().find(ea)->y != y0 + 60.0f) return fail("B has the released position");
    if (server.commits() != commits0 + 1) return fail("one commit for the whole drag");
    // A cancelled drag (Esc) puts B's copy back.
    a.begin_drag_move();
    a.workspace().update_move(0.0f, -45.0f);
    if (!pump(ticks, [&] { return b.workspace().find(ea)->y == y0 + 15.0f; }, 5)) {
      return fail("B sees the second drag");
    }
    a.cancel_drag_move();
    if (!pump(ticks, [&] { return !b.team().previewing(ea) && b.workspace().find(ea)->y == y0 + 60.0f; }, 5)) {
      return fail("cancelled drag restored on B");
    }
    if (!pump(ticks, converged, 5)) return fail("converge after the cancelled drag");
    std::cout << "[smoke] team: mid-drag preview OK (" << a.team().previews_sent() << " sent, "
              << b.team().previews_shown() << " shown)\n";

    // Take-over: while A is busy with the crate B can't take it; once A
    // has left it alone for lock_idle_s, B can.
    b.workspace().clear_selection();
    b.workspace().select(ea);
    b.team().take_over({ea});
    settle(ticks, 0.3);
    if (b.team().lock_holder(ea) != a.team().client().me()) return fail("busy lock not taken over");
    bool told = false;
    for (const std::string& n : b.team().recent_notes()) told |= n.find("still working") != std::string::npos;
    if (!told) return fail("refused take-over says why");
    settle(ticks, 1.0);
    b.team().take_over({ea});
    if (!pump(ticks, [&] { return a.team().lock_holder(ea) == b.team().client().me() && b.team().lock_holder(ea) == 0; }, 5)) {
      return fail("idle lock taken over");
    }
    b.begin_drag_move();
    b.workspace().update_move(0.0f, -60.0f);
    b.end_drag_move();
    if (!pump(ticks, converged, 5)) return fail("B edits the taken-over crate");
    if (a.workspace().find(ea)->y != y0) return fail("B's edit after take-over reached A");
    told = false;
    for (const std::string& n : a.team().recent_notes()) told |= n.find("took over") != std::string::npos;
    if (!told) return fail("A is told about the take-over");
    a.workspace().clear_selection();
    b.workspace().clear_selection();
    a.workspace().select(ea);  // back to A for the rest
    std::cout << "[smoke] team: lock take-over OK\n";
  }

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

// A Vault on a slow trail: every transfer takes a while (non-blocking test).
class SlowBackend : public ts::tombstone::vault::Backend {
 public:
  SlowBackend(std::shared_ptr<ts::tombstone::vault::Backend> inner, int ms) : in_(std::move(inner)), ms_(ms) {}
  std::string id() const override { return in_->id(); }
  std::string label() const override { return in_->label(); }
  bool remote() const override { return in_->remote(); }
  bool ping(std::string* i, std::string* e) override { return in_->ping(i, e); }
  bool list(const ts::tombstone::vault::Query& q, std::vector<ts::tombstone::vault::Item>* o,
            std::string* e) override {
    return in_->list(q, o, e);
  }
  bool get(const std::string& id, ts::tombstone::vault::Item* o, std::string* e) override {
    return in_->get(id, o, e);
  }
  bool missing(const std::vector<std::string>& s, std::vector<std::string>* o, std::string* e) override {
    return in_->missing(s, o, e);
  }
  bool upload(const std::string& p, const std::string& sha, std::uint64_t n, std::string* e) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
    return in_->upload(p, sha, n, e);
  }
  bool download(const std::string& sha, const std::string& d, std::string* e) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms_));
    return in_->download(sha, d, e);
  }
  bool publish(const ts::tombstone::vault::Item& m, const ts::tombstone::vault::Version& v,
               ts::tombstone::vault::PublishResult* o, std::string* e) override {
    return in_->publish(m, v, o, e);
  }
  bool update_item(const ts::tombstone::vault::Item& m, ts::tombstone::vault::Item* o, std::string* e) override {
    return in_->update_item(m, o, e);
  }
  bool remove_item(const std::string& id, std::string* e) override { return in_->remove_item(id, e); }
  bool remove_version(const std::string& id, int v, std::string* e) override {
    return in_->remove_version(id, v, e);
  }

 private:
  std::shared_ptr<ts::tombstone::vault::Backend> in_;
  int ms_;
};

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
    // Non-blocking: a slow Vault and a 3 MB file; the editors' frames
    // stay short while it travels and the progress is there to show.
    double worst_ms = 0.0;
    bool saw_progress = false;
    if (!rc) {
      a.team().set_asset_backend(std::make_shared<SlowBackend>(
          std::make_shared<tv::RemoteBackend>(url, key), 700));
      b.team().set_asset_backend(std::make_shared<SlowBackend>(
          std::make_shared<tv::RemoteBackend>(url, key), 700));
      {
        std::ofstream big(pa / "assets" / "canyon.bin", std::ios::binary);
        std::string chunk(1 << 16, 'x');
        for (int i = 0; i < 48; ++i) {
          chunk[0] = static_cast<char>(i);
          big << chunk;
        }
      }
      std::vector<Tick> timed = {[&](float) { server.poll(); },
                                 [&](float dt) {
                                   const auto t0 = std::chrono::steady_clock::now();
                                   a.tick_team(dt);
                                   b.tick_team(dt);
                                   const double ms = std::chrono::duration<double, std::milli>(
                                                         std::chrono::steady_clock::now() - t0).count();
                                   worst_ms = std::max(worst_ms, ms);
                                   const ed::TeamAssetProgress pa_ = a.team().asset_progress();
                                   const ed::TeamAssetProgress pb_ = b.team().asset_progress();
                                   if ((pa_.running && pa_.files_total > 0 && pa_.bytes_total >= (3u << 20)) ||
                                       (pb_.running && pb_.files_total > 0 && pb_.bytes_total >= (3u << 20))) {
                                     saw_progress = true;
                                   }
                                 }};
      const fs::path big_b = pb / "assets" / "canyon.bin";
      if (!pump(timed, [&] { return fs::exists(big_b) && fs::file_size(big_b) == (3u << 20); }, 20)) {
        rc = fail("3 MB file reached B through the slow Vault");
      } else if (!saw_progress) {
        rc = fail("transfer progress shown (files / bytes)");
      } else if (worst_ms > 150.0) {
        rc = fail("editor frame waited for the Vault (" + std::to_string(worst_ms) + " ms)");
      }
    }
    // Deletes: A removes the wanted poster; B's copy goes (kept in the trash).
    if (!rc) {
      settle(ticks, 2.5);  // both sides agree the file is in step
      fs::remove(pa / "assets" / "wanted.txt", ec);
      if (!pump(ticks, [&] { return !fs::exists(got); }, 15)) {
        rc = fail("asset delete reached B");
      } else if (!fs::exists(pb / ".tombstone" / "team" / "trash" / "assets" / "wanted.txt")) {
        rc = fail("deleted asset kept in B's team trash");
      } else {
        const tc::Docs* d = server.docs("_team_assets");
        if (!d || d->at("scene.json").assets.count("assets/wanted.txt")) rc = fail("delete reached the session");
      }
    }
    if (!rc) {
      std::cout << "[smoke] team assets: OK (A sent " << a.team().assets_sent() << ", B fetched "
                << b.team().assets_fetched() << ", A fetched " << a.team().assets_fetched()
                << ", B removed " << b.team().assets_removed() << "; slow Vault, worst frame "
                << static_cast<int>(worst_ms) << " ms)\n";
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
  // Deletes in 3D: B's delete of the sphere A holds is turned back ...
  b.select(cb);
  settle(ticks, 0.2);
  if (b.delete_selected() == 0) return fail("3D delete (optimistic)");
  if (!pump(ticks, [&] { return converged() && b.find(cb) != nullptr; }, 5)) {
    return fail("3D delete of a locked entity comes back");
  }
  // ... and A's own delete of it reaches B, lock and all.
  b.clear_selection();
  if (a.delete_selected() == 0) return fail("3D host delete");
  if (!pump(ticks, [&] { return converged() && !b.find(cb) && b.team().lock_holder(cb) == 0; }, 5)) {
    return fail("3D delete reached B and freed the lock");
  }
  if (a.team().server()->sessions().at(0).locks.count(tc::entity_lock("scene.json", cb))) {
    return fail("server dropped the deleted entity's lock");
  }
  // Terrain: sculpt / paint strokes travel as patches of the touched rect.
  {
    namespace terr = ts::tombstone::terrain;
    auto data_of = [](ed::Editor3DScreen& s, std::uint64_t id) -> std::shared_ptr<terr::TerrainData> {
      const auto* e = s.find(id);
      if (!e || !e->terrain) return nullptr;
      return terr::acquire(s.project().path, *e->terrain);
    };
    auto same_ground = [&](std::uint64_t id) {
      auto da = data_of(a, id), db = data_of(b, id);
      return da && db && da->heights == db->heights && da->splat == db->splat && da->foliage == db->foliage;
    };
    auto stroke = [&](ed::Editor3DScreen& s, std::uint64_t id, float x, float z) {
      if (!s.terrain_stroke_begin(id)) return false;
      for (int k = 0; k < 8; ++k) s.terrain_stroke({x + 0.2f * k, 0.0f, z}, 0.05f);
      s.terrain_stroke_end();
      return true;
    };
    const std::uint64_t tid = a.create_terrain(65, 64.0f, true, "meadow");
    if (!tid) return fail("3D terrain created");
    if (!pump(ticks, [&] { return converged() && b.find(tid) && same_ground(tid); }, 8)) {
      return fail("new terrain (hills) reached B");
    }
    const float h0 = data_of(b, tid)->h(32 + 3, 32 - 2);
    a.terrain_tool().mode = 0;
    a.terrain_tool().op = terr::BrushOp::Raise;
    a.terrain_tool().radius = 4.0f;
    a.terrain_tool().strength = 8.0f;
    const std::uint64_t sent0 = a.team().client().bytes_sent();
    if (!stroke(a, tid, 3.0f, -2.0f)) return fail("A sculpt stroke");
    if (!pump(ticks, [&] { return converged() && same_ground(tid); }, 8)) return fail("A's sculpt reached B");
    if (!(data_of(b, tid)->h(32 + 3, 32 - 2) > h0)) return fail("B's ground rose");
    const std::uint64_t sculpt_bytes = a.team().client().bytes_sent() - sent0;
    a.terrain_tool().mode = 1;
    a.terrain_tool().layer = 2;
    if (!stroke(a, tid, -10.0f, 10.0f)) return fail("A paint stroke");
    if (!pump(ticks, [&] { return converged() && same_ground(tid); }, 8)) return fail("A's paint reached B");
    // A holds the terrain (create selected it): B's stroke is turned back.
    b.terrain_tool().mode = 0;
    b.terrain_tool().op = terr::BrushOp::Lower;
    if (!stroke(b, tid, -12.0f, -12.0f)) return fail("B sculpt stroke");
    if (same_ground(tid)) return fail("B's stroke applied optimistically");
    if (!pump(ticks, [&] { return converged() && same_ground(tid); }, 8)) {
      return fail("B's stroke on A's terrain rolled back");
    }
    // Once A lets go B may sculpt, and A sees it.
    a.clear_selection();
    b.select(tid);
    if (!pump(ticks, [&] { return a.team().lock_holder(tid) == b.team().client().me(); }, 5)) {
      return fail("B holds the terrain");
    }
    const float ha = data_of(a, tid)->h(32 - 12, 32 - 12);
    if (!stroke(b, tid, -12.0f, -12.0f)) return fail("B sculpt stroke 2");
    if (!pump(ticks, [&] { return converged() && same_ground(tid); }, 8)) return fail("B's sculpt reached A");
    if (!(data_of(a, tid)->h(32 - 12, 32 - 12) < ha)) return fail("A's ground sank");
    b.clear_selection();
    std::cout << "[smoke] team terrain: OK (sculpt patch " << sculpt_bytes << " bytes)\n";
  }
  // Host leaves: the session ends for B.
  a.team().leave();
  if (!pump(ticks, [&] { return !b.team().joined(); }, 12)) return fail("host leaving ends the session");
  std::cout << "[smoke] team 3D (editor host): OK\n";
  return 0;
}

// ---- Persistent sessions: server restart, riding back in, editor restart ----

int check_persist() {
  const fs::path state = fs::absolute(fs::path("TombStoneProjects") / "_team_state");
  std::error_code ec;
  fs::remove_all(state, ec);
  tc::TeamServer server;
  tc::ServerOptions so;
  so.port = 0;
  so.state_dir = state.string();
  so.save_delay_s = 0.2;
  so.check_key = [](const std::string& key, std::string* name, bool* can_write) {
    if (key == "tsv_annie") *name = "Annie";
    else if (key == "tsv_wyatt") *name = "Wyatt";
    else return false;
    *can_write = true;
    return true;
  };
  std::string err;
  if (!server.start(so, &err)) return fail("persist server start: " + err);
  so.port = server.port();  // the restart comes back on the same port

  ed::Editor2DScreen a(fresh("_team_persist_a", ed::ProjectKind::TwoD));
  ed::Editor2DScreen b(fresh("_team_persist_b", ed::ProjectKind::TwoD));
  a.on_enter();
  b.on_enter();
  std::vector<Tick> ticks = {[&](float) { server.poll(); }, [&](float dt) { a.tick_team(dt); },
                             [&](float dt) { b.tick_team(dt); }};
  ed::TeamJoinSettings j;
  j.port = so.port;
  j.key = "tsv_annie";
  j.claim = "_team_persist_claim";
  if (!a.team().join(j, &err) || !pump(ticks, [&] { return a.team().joined(); }, 5)) return fail("persist A join");
  j.key = "tsv_wyatt";
  if (!b.team().join(j, &err) || !pump(ticks, [&] { return b.team().joined(); }, 5)) return fail("persist B join");
  auto converged = [&] {
    const tc::Docs* d = server.docs("_team_persist_claim");
    if (!d || !d->count("scene.json")) return false;
    const std::string truth = d->at("scene.json").canonical();
    return a.team().joined() && b.team().joined() && a.team().client().idle() && b.team().client().idle() &&
           doc_of(a.team()) == truth && doc_of(b.team()) == truth;
  };
  const std::uint64_t wagon = a.create_entity("Wagon");
  if (!pump(ticks, converged, 5)) return fail("persist: first edit");
  if (!pump(ticks, [&] { return server.saves() > 0 && fs::exists(state / "_team_persist_claim.team.json"); }, 3)) {
    return fail("session saved to the state dir");
  }
  const std::uint32_t a_id = a.team().client().me(), b_id = b.team().client().me();

  // The server goes down. Both editors keep working.
  server.stop();
  if (!pump(ticks, [&] { return a.team().reconnecting() && b.team().reconnecting(); }, 5)) {
    return fail("editors notice the lost server and ride back in");
  }
  a.workspace().find(wagon)->x += 50.0f;  // offline edit (no undo step needed)
  const float wagon_x = a.workspace().find(wagon)->x;
  const std::uint64_t fire = b.create_entity("Campfire");
  settle(ticks, 0.5);

  // Back up from its state dir: same claim, same seq, riders come back.
  if (!server.start(so, &err)) return fail("persist server restart: " + err);
  if (server.sessions().empty() || server.sessions().at(0).seq == 0) return fail("session loaded from disk");
  if (!pump(ticks, [&] { return converged() && a.team().rejoins() == 1 && b.team().rejoins() == 1; }, 25)) {
    return fail("both rode back in and converged");
  }
  if (a.team().client().me() != a_id || b.team().client().me() != b_id) return fail("same riders after the restart");
  if (!a.team().client().rode_back() || !b.team().client().rode_back()) return fail("server knew the rejoin tokens");
  if (!a.workspace().find(fire) || !b.workspace().find(wagon) || b.workspace().find(wagon)->x != wagon_x) {
    return fail("offline edits from both sides survived");
  }
  std::cout << "[smoke] team: server restart + ride back in OK (tries A " << a.team().reconnect_tries() << ", B "
            << b.team().reconnect_tries() << ")\n";

  // Editor restart: A leaves, a fresh editor on the same project rides
  // back in from .tombstone/team/last.json as the same rider.
  const ed::ProjectInfo pa = a.team().project();
  a.team().leave();
  if (!pump(ticks, [&] { return !b.team().client().member(a_id); }, 5)) return fail("A rode off");
  ticks[1] = [](float) {};
  ed::Editor2DScreen a2(pa);
  a2.on_enter();
  ticks.push_back([&](float dt) { a2.tick_team(dt); });
  if (!a2.team().has_last_ride() || a2.team().last_ride_hosted()) return fail("last ride remembered");
  ed::TeamJoinSettings j2;
  j2.host = a2.team().ui.host_addr;
  j2.port = static_cast<std::uint16_t>(a2.team().ui.join_port);
  j2.user = a2.team().ui.user;
  j2.key = "tsv_annie";  // never saved: typed again
  j2.claim = "_team_persist_claim";
  if (!a2.team().join(j2, &err) || !pump(ticks, [&] { return a2.team().joined(); }, 5)) {
    return fail("restarted editor rides back in");
  }
  if (a2.team().client().me() != a_id || !a2.team().client().rode_back()) return fail("same rider after an editor restart");
  b.team().leave();
  a2.team().leave();
  server.stop();
  std::cout << "[smoke] team: editor restart rides back in as member " << a_id << " OK\n";
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
  if (rc == 0) rc = check_persist();
  net::net_shutdown();
  if (rc == 0) std::cout << "[smoke] team (Live Team Claims): OK\n";
  return rc;
}

}  // namespace tombstone
}  // namespace ts
