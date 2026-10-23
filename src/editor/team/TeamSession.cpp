#include "editor/team/TeamSession.h"

#include "render3d/Scene3D.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

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
    : binding_(std::move(binding)), project_(std::move(project)) {
  load_last_ride();
}

std::string TeamSession::team_dir() const {
  if (project_.path.empty()) return {};
  return (std::filesystem::u8path(project_.path) / ".tombstone" / "team").string();
}

void TeamSession::save_last_ride() const {
  const std::string dir = team_dir();
  if (dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  team::Value o = team::Value::object();
  o.set("mode", team::Value::string(last_join_host_ ? "host" : "join"));
  o.set("host", team::Value::string(last_join_.host));
  o.set("port", team::Value::number(last_join_host_ && server_ ? server_->port() : last_join_.port));
  o.set("user", team::Value::string(last_join_.user));
  o.set("claim", team::Value::string(client_.session().str("claim", last_join_.claim)));
  o.set("vault_url", team::Value::string(ui.vault_url));
  o.set("rejoin", team::Value::string(client_.rejoin_token()));
  std::ofstream f(std::filesystem::u8path(dir) / "last.json", std::ios::binary | std::ios::trunc);
  f << team::write_compact(o);
}

void TeamSession::load_last_ride() {
  const std::string dir = team_dir();
  if (dir.empty()) return;
  std::ifstream f(std::filesystem::u8path(dir) / "last.json", std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  team::Value o;
  std::string err;
  if (!json::parse(ss.str(), &o, &err) || !o.is_object()) return;
  has_last_ride_ = true;
  last_ride_host_ = o.str("mode") == "host";
  saved_rejoin_ = o.str("rejoin");
  auto copy = [](char* dst, std::size_t n, const std::string& v) {
    std::snprintf(dst, n, "%s", v.c_str());
  };
  ui.mode = last_ride_host_ ? 1 : 0;
  const int port = static_cast<int>(o.num("port", kDefaultPort));
  if (last_ride_host_) ui.host_port = port;
  else ui.join_port = port;
  if (!last_ride_host_) copy(ui.host_addr, sizeof(ui.host_addr), o.str("host", "127.0.0.1"));
  copy(ui.user, sizeof(ui.user), o.str("user"));
  copy(ui.vault_url, sizeof(ui.vault_url), o.str("vault_url"));
}

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
  // The claim's session outlives the editor: it is kept beside the project.
  if (!team_dir().empty()) so.state_dir = (std::filesystem::u8path(team_dir()) / "host").string();
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
  o.rejoin = saved_rejoin_;
  loaded_ = false;
  if (!s.vault_url.empty()) {
    assets_ = std::make_shared<vault::RemoteBackend>(s.vault_url, s.vault_key);
  }
  last_join_ = o;
  last_join_host_ = true;
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
  if (!s.vault_url.empty()) assets_ = std::make_shared<vault::RemoteBackend>(s.vault_url, s.key);
  // Seeds a session only if nobody is riding this claim yet (ts_vault).
  if (o.claim != "*") o.seed[binding_->scene_key()] = binding_->capture();
  o.rejoin = saved_rejoin_;
  loaded_ = false;
  last_join_ = o;
  last_join_host_ = false;
  return client_.join(o, error);
}

void TeamSession::leave() {
  was_joined_ = false;
  reconnecting_ = false;
  rejoin_pending_ = false;
  asset_sync_.cancel_and_wait();
  client_.leave();
  if (server_) {
    // Let the others hear the host is gone.
    server_->poll();
    server_->stop();
    server_.reset();
  }
  if (!remote_preview_.empty()) {
    std::vector<std::uint64_t> ids;
    for (const auto& [id, p] : remote_preview_) ids.push_back(id);
    binding_->end_preview(ids);
  }
  binding_->set_id_range(0, 0);
  held_.clear();
  loaded_ = false;
  files_.clear();
  synced_.clear();
  asset_sent_.clear();
  my_preview_ids_.clear();
  remote_preview_.clear();
  preview_closed_.clear();
  last_preview_.clear();
}

void TeamSession::drop_now() {
  was_joined_ = false;
  reconnecting_ = false;
  rejoin_pending_ = false;
  asset_sync_.cancel_and_wait();
  client_.drop_now();
  binding_->set_id_range(0, 0);
  held_.clear();
  loaded_ = false;
}

bool TeamSession::active() const {
  return client_.state() == team::TeamClient::State::Connecting || client_.joined() || reconnecting_;
}

void TeamSession::start_reconnect() {
  reconnecting_ = true;
  rejoin_pending_ = true;
  reconnect_clock_ = 0.0;
  reconnect_wait_ = 1.0;
  reconnect_tries_ = 0;
  auto it = client_.confirmed().find(binding_->scene_key());
  rejoin_base_ = it == client_.confirmed().end() ? team::SceneDoc{} : it->second;
  // Ride back in to the claim we were on, as ourselves.
  last_join_.claim = client_.session().str("claim", last_join_.claim);
  last_join_.rejoin = client_.rejoin_token();
  asset_sync_.cancel_and_wait();
  if (!remote_preview_.empty()) {
    std::vector<std::uint64_t> ids;
    for (const auto& [id, p] : remote_preview_) ids.push_back(id);
    binding_->end_preview(ids);
  }
  remote_preview_.clear();
  preview_closed_.clear();
  my_preview_ids_.clear();
  last_preview_.clear();
  held_.clear();
  loaded_ = false;
  notes_.push_back("Lost the trail to the posse. Riding back in; keep working, your edits will follow.");
}

void TeamSession::tick_reconnect(double dt) {
  using S = team::TeamClient::State;
  if (client_.state() == S::Refused) {
    reconnecting_ = false;
    rejoin_pending_ = false;
    notes_.push_back("Couldn't ride back in: " + client_.error());
    return;
  }
  if (client_.state() == S::Connecting) return;
  reconnect_clock_ += dt;
  if (reconnect_clock_ < reconnect_wait_) return;
  reconnect_clock_ = 0.0;
  reconnect_wait_ = std::min(reconnect_wait_ * 2.0, 15.0);
  ++reconnect_tries_;
  team::JoinOptions o = last_join_;
  o.seed.clear();
  // If the host lost the claim (restarted without its state), ours seeds it.
  o.seed[binding_->scene_key()] = binding_->capture();
  std::string err;
  client_.join(o, &err);
}

void TeamSession::rode_back_in(const team::Docs& docs) {
  const std::string scene = binding_->scene_key();
  auto strip = [](team::SceneDoc d) {
    d.assets.clear();  // files sync on their own
    return d;
  };
  const team::SceneDoc base = strip(rejoin_base_);
  auto it = docs.find(scene);
  const team::SceneDoc server = strip(it == docs.end() ? team::SceneDoc{} : it->second);
  const team::SceneDoc live = strip(binding_->capture());
  // Ours: everything not confirmed when the line went dead (sent but
  // unanswered, and edits made while away). Theirs: what the posse did.
  const std::vector<team::Op> mine = team::diff(base, live);
  const std::vector<team::Op> theirs = team::diff(base, server);
  std::string label;
  binding_->take_local_ops(&label);  // the shadow is the live scene now
  if (!theirs.empty()) {
    binding_->apply(theirs);
    if (!mine.empty()) binding_->apply(mine);  // ours land on top, as they will on the server
  }
  if (!mine.empty()) {
    if (client_.can_write()) {
      client_.submit(scene, "Edits while away", mine);
      ++sent_;
    } else {
      resync_clock_ = 1e9;
    }
  }
  ++rejoins_;
  notes_.push_back(std::string(client_.rode_back() ? "Rode back in as " : "Rode back in (as a new rider) as ") +
                   client_.my_name() + ": " + std::to_string(mine.size()) + " change(s) from while you were away, " +
                   std::to_string(theirs.size()) + " from the posse.");
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
  for (const std::string& n : out) {
    recent_notes_.push_back(n);
    if (recent_notes_.size() > 32) recent_notes_.pop_front();
  }
  return out;
}

std::string TeamSession::status_line() const {
  using S = team::TeamClient::State;
  if (reconnecting_) {
    return "Lost the trail: riding back in" +
           (reconnect_tries_ ? " (try " + std::to_string(reconnect_tries_) + ")" : std::string()) + "...";
  }
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
    // Lost (not left, not refused): ride back in. The host's own editor
    // never loses its in-process server, so only joiners retry.
    if (was_joined_ && !server_ && client_.state() == team::TeamClient::State::Closed) start_reconnect();
    was_joined_ = false;
    if (reconnecting_) tick_reconnect(dt);
    if (server_ && !active()) {
      server_->stop();
      server_.reset();
    }
    return;
  }
  was_joined_ = true;
  const std::string scene = binding_->scene_key();

  team::Docs docs;
  if (client_.take_welcome(&docs)) {
    binding_->set_id_range(client_.id_lo(), client_.id_hi());
    auto it = docs.find(scene);
    if (rejoin_pending_) {
      rejoin_pending_ = false;
      reconnecting_ = false;
      rode_back_in(docs);
      loaded_ = true;
      held_.clear();
      first_asset_pass_ = false;  // ours changed while away: compare, don't overwrite
      asset_clock_ = 2.0;
      save_last_ride();
      send_presence(true);
    } else if (it != docs.end()) {
      binding_->load(it->second);
    } else {
      // Session without this scene yet: ours becomes it.
      std::string label;
      binding_->take_local_ops(&label);
      const team::SceneDoc mine = binding_->capture();
      client_.submit(scene, "Seed " + scene, team::diff(team::SceneDoc{}, mine));
    }
    if (loaded_) {
      // (rode back in above)
    } else {
      loaded_ = true;
      held_.clear();
      first_asset_pass_ = true;
      asset_clock_ = 2.0;  // first file pass on the next tick
      notes_.push_back(client_.session().str("claim_name", "the claim") + ": you ride as " +
                       client_.my_name() + (client_.rode_back() ? " (welcome back)." : "."));
      save_last_ride();
      send_presence(true);
    }
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
      forget_previews(ops);
      binding_->apply(ops);
      ++remote_applied_;
    }
  }
  // 2b. Mid-drag previews, both ways.
  tick_previews(dt, busy);
  // 3. Safety net: idle and nothing pending -> must match the server.
  resync_clock_ += dt;
  if (!busy && client_.idle() && held_.empty() && remote_preview_.empty() && resync_clock_ >= 1.0) {
    resync_clock_ = 0.0;
    const team::SceneDoc live = binding_->capture();
    auto it = client_.confirmed().find(scene);
    const team::SceneDoc empty;
    team::SceneDoc truth = it == client_.confirmed().end() ? empty : it->second;
    truth.assets.clear();  // files sync on their own (sync_assets)
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
  // 3b. Project files through the Vault (a worker does the hauling).
  finish_asset_pass();
  asset_clock_ += dt;
  if (assets_ && !busy && asset_clock_ >= 2.0 && !asset_sync_.running()) {
    asset_clock_ = 0.0;
    sync_assets();
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

void TeamSession::forget_previews(const std::vector<team::Op>& ops) {
  for (const team::Op& op : ops) {
    if (op.kind == team::Op::Kind::Put || op.kind == team::Op::Kind::Del) remote_preview_.erase(op.id);
  }
}

void TeamSession::tick_previews(double dt, bool busy) {
  const std::string scene = binding_->scene_key();
  // Ours: while a drag / Inspector edit is open, what it does so far.
  if (busy && client_.can_write()) {
    preview_clock_ += dt;
    if (preview_clock_ >= 0.05) {  // ~20 Hz
      preview_clock_ = 0.0;
      std::vector<team::Op> puts;
      for (team::Op& op : binding_->preview_ops()) {
        if (client_.lock_owner(team::entity_lock(scene, op.id)) == client_.me()) puts.push_back(std::move(op));
      }
      if (!puts.empty()) {
        team::Value arr = team::Value::array();
        for (const team::Op& op : puts) arr.push(team::op_to_json(op));
        const std::string text = team::write_compact(arr);
        if (text != last_preview_) {
          last_preview_ = text;
          client_.send_preview(scene, puts);
          ++previews_sent_;
          for (const team::Op& op : puts) {
            if (std::find(my_preview_ids_.begin(), my_preview_ids_.end(), op.id) == my_preview_ids_.end()) {
              my_preview_ids_.push_back(op.id);
            }
          }
        }
      }
    }
  } else if (!busy && !my_preview_ids_.empty()) {
    // Let go: the commit (if anything changed) went out above, on the
    // same ordered channel, so this end lands after it.
    client_.end_preview(scene, my_preview_ids_);
    my_preview_ids_.clear();
    last_preview_.clear();
    preview_clock_ = 0.0;
  }

  // Theirs.
  for (team::PreviewEvent& e : client_.take_previews()) {
    if (e.scene != scene) continue;
    if (e.end) {
      std::uint64_t& closed = preview_closed_[e.member];
      closed = std::max(closed, e.n);
      std::vector<std::uint64_t> ids;
      for (std::uint64_t id : e.ids) {
        auto it = remote_preview_.find(id);
        if (it == remote_preview_.end() || it->second.member != e.member) continue;
        bool landing = false;  // a held commit will set it anyway
        for (const auto& [sc, op] : held_) landing |= sc == scene && op.id == id;
        remote_preview_.erase(it);
        if (!landing) ids.push_back(id);
      }
      if (!ids.empty()) binding_->end_preview(ids);
      continue;
    }
    auto closed = preview_closed_.find(e.member);
    if (closed != preview_closed_.end() && e.n <= closed->second) continue;  // late packet
    std::vector<team::Op> puts;
    for (team::Op& op : e.ops) {
      if (client_.lock_owner(team::entity_lock(scene, op.id)) == client_.me()) continue;
      remote_preview_[op.id] = RemotePreview{e.member, 0.0};
      puts.push_back(std::move(op));
    }
    if (!puts.empty()) {
      binding_->apply_preview(puts);
      ++previews_shown_;
    }
  }
  // A preview whose sender went quiet (or rode off) is put back.
  std::vector<std::uint64_t> stale;
  for (auto it = remote_preview_.begin(); it != remote_preview_.end();) {
    it->second.age += dt;
    if (it->second.age > 3.0 || !client_.member(it->second.member)) {
      stale.push_back(it->first);
      it = remote_preview_.erase(it);
    } else {
      ++it;
    }
  }
  if (!stale.empty()) binding_->end_preview(stale);
}

void TeamSession::take_over(const std::vector<std::uint64_t>& ids) {
  std::vector<std::string> keys;
  for (std::uint64_t id : ids) keys.push_back(team::entity_lock(binding_->scene_key(), id));
  client_.take_locks(keys);
}

std::vector<std::uint64_t> TeamSession::selection_held_by_others() const {
  std::vector<std::uint64_t> out;
  if (!binding_ || !client_.joined()) return out;
  for (std::uint64_t id : binding_->selection()) {
    if (holder_member(id)) out.push_back(id);
  }
  return out;
}

bool TeamSession::sync_assets() {
  if (!assets_ || !client_.joined() || !loaded_) return false;
  finish_asset_pass();
  if (asset_sync_.running()) return false;
  const std::string scene = binding_->scene_key();
  TeamAssetPass p;
  p.root = project_.path;
  auto cit = client_.confirmed().find(scene);
  if (cit != client_.confirmed().end()) p.session = cit->second.assets;
  p.files = files_;
  p.synced = synced_;
  p.sent = asset_sent_;
  // Nothing in the saddlebag: a send the session never confirmed was
  // lost or turned back, so it may go again.
  if (client_.idle()) p.sent.clear();
  p.first = first_asset_pass_;
  p.can_write = client_.can_write();
  if (!asset_sync_.start(std::move(p), assets_)) return false;
  first_asset_pass_ = false;
  return true;
}

void TeamSession::finish_asset_pass() {
  TeamAssetPass r;
  if (!asset_sync_.take(&r)) return;
  files_ = std::move(r.files);
  synced_ = std::move(r.synced);
  asset_sent_ = std::move(r.sent);
  assets_up_ += r.up;
  assets_down_ += r.down;
  assets_removed_ += r.removed;
  notes_.insert(notes_.end(), r.notes.begin(), r.notes.end());
  if (!r.ops.empty() && client_.joined()) {
    std::size_t dels = 0;
    for (const team::Op& op : r.ops) dels += op.sha256.empty();
    std::string label = r.ops.size() == 1 ? (dels ? "Delete " : "Asset ") + r.ops.front().key
                                          : "Assets (" + std::to_string(r.ops.size()) + ")";
    client_.submit(binding_->scene_key(), label, std::move(r.ops));
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
