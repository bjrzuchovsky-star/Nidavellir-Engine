#include "editor/team/TeamSession.h"

#include "render3d/Scene3D.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifndef TS_ENGINE_VERSION
#define TS_ENGINE_VERSION "0.0.0"
#endif

namespace ts {
namespace tombstone {
namespace editor {

namespace {

const char* dim_of(const ProjectInfo& p) {
  if (p.kind == ProjectKind::TwoD) return "2d";
  return vr_enabled(p) ? "vr" : "3d";
}

}  // namespace

TeamSession::TeamSession(std::unique_ptr<TeamBinding> binding, ProjectInfo project)
    : binding_(std::move(binding)), project_(std::move(project)) {}

TeamSession::~TeamSession() { leave(); }

team::JoinOptions TeamSession::base_join() const {
  team::JoinOptions o;
  o.claim = project_.id;
  o.claim_name = project_.name;
  o.engine = TS_ENGINE_VERSION;
  o.scene2d = scene_json::kSceneVersion;
  o.scene3d = scene3d::kScene3DVersion;
  o.dim = dim_of(project_);
  o.scene = binding_->scene_key();
  return o;
}

bool TeamSession::host(const TeamHostSettings& s, std::string* error) {
  leave();
  server_ = std::make_unique<team::TeamServer>();
  team::ServerOptions so;
  so.port = s.port;
  so.only_claim = project_.id;
  so.passcode = s.pass;
  so.engine = TS_ENGINE_VERSION;
  if (!server_->start(so, error)) {
    server_.reset();
    return false;
  }
  team::JoinOptions o = base_join();
  o.host = "127.0.0.1";
  o.port = server_->port();
  o.user = s.user;
  o.pass = s.pass;
  o.seed[binding_->scene_key()] = binding_->capture();
  loaded_ = false;
  if (!client_.join(o, error)) {
    server_.reset();
    return false;
  }
  return true;
}

bool TeamSession::join(const TeamJoinSettings& s, std::string* error) {
  leave();
  team::JoinOptions o = base_join();
  o.host = s.host;
  o.port = s.port;
  o.user = s.user;
  o.key = s.key;
  o.pass = s.pass;
  o.claim = s.claim.empty() ? "*" : s.claim;
  o.wrap_transport = s.wrap_transport;
  // Seeds a session only if nobody is riding this claim yet (ts_vault).
  if (o.claim != "*") o.seed[binding_->scene_key()] = binding_->capture();
  loaded_ = false;
  return client_.join(o, error);
}

void TeamSession::leave() {
  client_.leave();
  if (server_) {
    // Let the others hear the host is gone.
    server_->poll();
    server_->stop();
    server_.reset();
  }
  binding_->set_id_range(0, 0);
  held_.clear();
  loaded_ = false;
}

void TeamSession::drop_now() {
  client_.drop_now();
  binding_->set_id_range(0, 0);
  held_.clear();
  loaded_ = false;
}

bool TeamSession::active() const {
  return client_.state() == team::TeamClient::State::Connecting || client_.joined();
}

void TeamSession::set_cursor(float x, float y, float z, bool valid) {
  if (cursor_pinned_) return;
  cursor_[0] = x;
  cursor_[1] = y;
  cursor_[2] = z;
  cursor_valid_ = valid;
}

std::uint32_t TeamSession::lock_holder(std::uint64_t id) const {
  if (!client_.joined()) return 0;
  const std::uint32_t who = client_.lock_owner(team::entity_lock(binding_->scene_key(), id));
  return who == client_.me() ? 0 : who;
}

const team::RemoteMember* TeamSession::holder_member(std::uint64_t id) const {
  const std::uint32_t who = lock_holder(id);
  return who ? client_.member(who) : nullptr;
}

std::vector<std::string> TeamSession::take_notes() {
  std::vector<std::string> out = client_.take_notes();
  out.insert(out.end(), notes_.begin(), notes_.end());
  notes_.clear();
  return out;
}

std::string TeamSession::status_line() const {
  using S = team::TeamClient::State;
  switch (client_.state()) {
    case S::Idle: return "Riding solo";
    case S::Connecting: return "Riding out to " + client_.options().host + "...";
    case S::Refused: return "Turned away: " + client_.error();
    case S::Closed: return client_.error().empty() ? "Riding solo" : client_.error();
    case S::Joined: break;
  }
  std::string s = std::string(hosting() ? "Hosting" : "Riding with") + " a posse of " +
                  std::to_string(client_.members().size() + 1);
  if (client_.pending()) s += " (" + std::to_string(client_.pending()) + " edit(s) in the saddlebag)";
  return s;
}

void TeamSession::send_presence(bool force) {
  team::Value p = team::Value::object();
  p.set("scene", team::Value::string(binding_->scene_key()));
  p.set("space", team::Value::string(binding_->space()));
  if (cursor_valid_) p.set("cursor", json::floats(cursor_, 3));
  team::Value sel = team::Value::array();
  for (std::uint64_t id : binding_->selection()) sel.push(team::id_value(id));
  p.set("sel", std::move(sel));
  const std::string text = team::write_compact(p);
  if (!force && text == last_presence_) return;
  last_presence_ = text;
  client_.send_presence(std::move(p));
}

void TeamSession::tick(double dt) {
  if (server_) server_->poll();
  client_.poll();
  if (!client_.joined()) {
    if (server_ && !active()) {
      server_->stop();
      server_.reset();
    }
    return;
  }
  const std::string scene = binding_->scene_key();

  team::Docs docs;
  if (client_.take_welcome(&docs)) {
    binding_->set_id_range(client_.id_lo(), client_.id_hi());
    auto it = docs.find(scene);
    if (it != docs.end()) {
      binding_->load(it->second);
    } else {
      // Session without this scene yet: ours becomes it.
      std::string label;
      binding_->take_local_ops(&label);
      const team::SceneDoc mine = binding_->capture();
      client_.submit(scene, "Seed " + scene, team::diff(team::SceneDoc{}, mine));
    }
    loaded_ = true;
    held_.clear();
    notes_.push_back(client_.session().str("claim_name", "the claim") + ": you ride as " +
                     client_.my_name() + ".");
    send_presence(true);
  }
  if (!loaded_) return;

  const bool busy = binding_->busy();
  // 1. Our committed steps go out.
  if (!busy) {
    std::string label;
    std::vector<team::Op> ops = binding_->take_local_ops(&label);
    if (!ops.empty()) {
      if (client_.can_write()) {
        client_.submit(scene, label.empty() ? "Edit" : label, std::move(ops));
        ++sent_;
      } else {
        notes_.push_back("Read-only key: your edit was put back.");
        resync_clock_ = 1e9;  // repair now (below)
      }
    }
  }
  // 2. Remote batches come in (they wait while we are mid-edit).
  for (auto& so : client_.take_apply()) held_.push_back(std::move(so));
  if (!busy && !held_.empty()) {
    std::vector<team::Op> ops;
    for (auto& [sc, op] : held_) {
      if (sc == scene) ops.push_back(std::move(op));
    }
    held_.clear();
    if (!ops.empty()) {
      binding_->apply(ops);
      ++remote_applied_;
    }
  }
  // 3. Safety net: idle and nothing pending -> must match the server.
  resync_clock_ += dt;
  if (!busy && client_.idle() && held_.empty() && resync_clock_ >= 1.0) {
    resync_clock_ = 0.0;
    const team::SceneDoc live = binding_->capture();
    auto it = client_.confirmed().find(scene);
    const team::SceneDoc empty;
    const team::SceneDoc& truth = it == client_.confirmed().end() ? empty : it->second;
    if (!live.same_as(truth)) {
      const std::vector<team::Op> fix = team::diff(live, truth);
      if (std::getenv("TS_TEAM_DEBUG")) {
        for (const team::Op& op : fix) {
          std::fprintf(stderr, "[team] resync %s: %s\n", op.touch_key().c_str(),
                       team::write_compact(team::op_to_json(op)).c_str());
        }
      }
      binding_->apply(fix);
      ++resyncs_;
    }
  }
  // 4. Locks = the selection.
  std::vector<std::string> keys;
  for (std::uint64_t id : binding_->selection()) keys.push_back(team::entity_lock(scene, id));
  client_.want_locks(std::move(keys));
  // 5. Presence at up to 10 Hz, heartbeat every second (keeps locks).
  presence_clock_ += dt;
  heartbeat_clock_ += dt;
  if (presence_clock_ >= 0.1) {
    presence_clock_ = 0.0;
    const bool beat = heartbeat_clock_ >= 1.0;
    if (beat) heartbeat_clock_ = 0.0;
    send_presence(beat);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
