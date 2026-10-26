#pragma once

// Live Team Claims in the editor (docs/team-claims.md). A TeamSession
// glues a team::TeamClient (and, for "Host from my editor", an in-process
// team::TeamServer) to one editor screen through a TeamBinding:
//
//   - every committed undo step becomes one op batch (the binding diffs the
//     live scene against its shadow while the editor is idle),
//   - remote batches are applied to the scene and rebased into the undo
//     history, so Ctrl+Z only reverts your own steps,
//   - the selection is the soft-lock set, the viewport cursor and the
//     selection travel as presence,
//   - once a second an idle editor checks itself against the server state.

#include "editor/ProjectInfo.h"
#include "editor/team/TeamAssets.h"
#include "team/TeamClient.h"
#include "team/TeamServer.h"
#include "vault/VaultBackend.h"

#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class Editor2DScreen;
class Editor3DScreen;

// One editor screen as the team layer sees it.
class TeamBinding {
 public:
  virtual ~TeamBinding() = default;
  virtual std::string scene_key() const { return "scene.json"; }
  // Mid-drag, an open Inspector edit, a brush stroke, playing: remote
  // batches wait and local changes are not sent yet.
  virtual bool busy() const = 0;
  // The live scene as a team doc.
  virtual team::SceneDoc capture() const = 0;
  // Replace the scene (joining a session). Clears undo history.
  virtual void load(const team::SceneDoc& doc) = 0;
  // Ops for what changed since the last call (live vs shadow); updates
  // the shadow. label = the undo step that made them.
  virtual std::vector<team::Op> take_local_ops(std::string* label) = 0;
  // Apply ops to the live scene, the shadow and every history state.
  virtual void apply(const std::vector<team::Op>& ops) = 0;
  virtual void set_id_range(std::uint64_t lo, std::uint64_t hi) = 0;
  virtual std::vector<std::uint64_t> selection() const = 0;
  virtual const char* space() const = 0;  // "2d" / "3d"

  // Mid-drag preview (docs/team-claims.md, 3.7). preview_ops: puts for
  // existing entities that differ from the shadow right now (mid-drag),
  // without taking them. apply_preview: show another member's transient
  // puts in the live scene only (not the shadow, not the history).
  // end_preview: put those entities back to the shadow. Entities under a
  // remote preview are left out of take_local_ops.
  virtual std::vector<team::Op> preview_ops() const { return {}; }
  virtual void apply_preview(const std::vector<team::Op>& puts) { (void)puts; }
  virtual void end_preview(const std::vector<std::uint64_t>& ids) { (void)ids; }
};

std::unique_ptr<TeamBinding> make_team_binding(Editor2DScreen& screen);
std::unique_ptr<TeamBinding> make_team_binding(Editor3DScreen& screen);

struct TeamHostSettings {
  std::uint16_t port = 24650;  // UDP
  std::string pass;            // optional passcode
  std::string user;            // display name
  std::string vault_url;       // optional: project files travel through this Vault
  std::string vault_key;
};

struct TeamJoinSettings {
  std::string host = "127.0.0.1";
  std::uint16_t port = 24650;
  std::string key;   // Vault API key (ts_vault --team)
  std::string pass;  // passcode (editor-hosted)
  std::string user;
  std::string claim = "*";  // "*" = whatever the host is running
  // Project files travel through this Vault ("" = the API key's server is
  // not known: assets stay local). ts_vault --team: http://host:<http port>.
  std::string vault_url;
  // Tests: wrap the transport (net::SimTransport latency).
  std::function<std::unique_ptr<net::Transport>(std::unique_ptr<net::Transport>)> wrap_transport;
};

// Team panel form state (kept with the session so it survives the panel).
struct TeamUiState {
  char host_addr[128] = "127.0.0.1";
  int host_port = 24650;
  int join_port = 24650;
  char key[128] = "";
  char pass[64] = "";
  char user[64] = "";
  char vault_url[160] = "";  // optional: files travel through this Vault
  int mode = 0;  // 0 = join, 1 = host
  std::string last_error;
};

class SessionBlobStore;

class TeamSession {
 public:
  TeamUiState ui;
  TeamSession(std::unique_ptr<TeamBinding> binding, ProjectInfo project);
  ~TeamSession();

  bool host(const TeamHostSettings& s, std::string* error);
  bool join(const TeamJoinSettings& s, std::string* error);
  void leave();
  // Tests: vanish without saying goodbye (a crashed editor).
  void drop_now();
  void tick(double dt);

  bool active() const;   // connecting or joined
  bool joined() const { return client_.joined(); }
  bool hosting() const { return server_ != nullptr; }
  team::TeamClient& client() { return client_; }
  const team::TeamClient& client() const { return client_; }
  team::TeamServer* server() { return server_.get(); }
  TeamBinding& binding() { return *binding_; }
  const ProjectInfo& project() const { return project_; }

  // The viewport's cursor in scene space (presence); valid = over it.
  void set_cursor(float x, float y, float z, bool valid);
  // Screenshots / demos: hold the cursor here (set_cursor is ignored).
  void pin_cursor(float x, float y, float z) {
    set_cursor(x, y, z, true);
    cursor_pinned_ = true;
  }
  // Another member holding entity `id` (0 = free or ours).
  std::uint32_t lock_holder(std::uint64_t id) const;
  const team::RemoteMember* holder_member(std::uint64_t id) const;

  // Assets (docs/team-claims.md, 4): project files under assets/ go up to
  // the Vault by SHA-256, come down where they differ, and deletes travel
  // both ways. A pass every 2 s while joined, on a worker thread: the
  // frame never waits for the Vault. sync_assets() starts a pass now
  // (false if one is still running).
  void set_asset_backend(std::shared_ptr<vault::Backend> backend) { assets_ = std::move(backend); }
  bool has_asset_backend() const { return assets_ != nullptr; }
  bool sync_assets();
  bool assets_busy() const { return asset_sync_.running(); }
  TeamAssetProgress asset_progress() const { return asset_sync_.progress(); }
  // Where files travel for this ride, in words ("through the Vault at
  // http://...", "over the team session"), and why they can't ("" = fine).
  const std::string& files_route() const { return files_route_; }
  const std::string& files_error() const { return files_error_; }
  // Project paths the file passes wrote or removed since the last call: the
  // editor drops its cached textures / models for them and rescans assets.
  std::vector<std::string> take_arrived();
  // Settings > Vault fills in a missing Vault URL / key (off in smokes, so
  // they never read the PC's own settings).
  static bool settings_fallback;
  std::uint64_t assets_sent() const { return assets_up_; }
  std::uint64_t assets_fetched() const { return assets_down_; }
  std::uint64_t assets_removed() const { return assets_removed_; }

  std::vector<std::string> take_notes();
  // The last few notes handed out (the panel log; smokes check them).
  const std::deque<std::string>& recent_notes() const { return recent_notes_; }
  std::string status_line() const;
  std::uint64_t resyncs() const { return resyncs_; }
  // Mid-drag previews: sent by this editor / shown from others.
  std::uint64_t previews_sent() const { return previews_sent_; }
  std::uint64_t previews_shown() const { return previews_shown_; }
  bool previewing(std::uint64_t id) const { return remote_preview_.count(id) != 0; }
  // Take over the entities in `ids` that someone else has left idle.
  void take_over(const std::vector<std::uint64_t>& ids);
  // Selected entities someone else holds (for the Take over button).
  std::vector<std::uint64_t> selection_held_by_others() const;
  // Riding back in (docs/team-claims.md, 3.8): a lost connection retries
  // with backoff (1, 2, 4 .. 15 s) as the same rider; edits made while away
  // go up once the session answers. Leave() stops it.
  bool reconnecting() const { return reconnecting_; }
  int reconnect_tries() const { return reconnect_tries_; }
  std::uint64_t rejoins() const { return rejoins_; }
  // The last ride on this project (.tombstone/team/last.json, no keys or
  // passcodes): prefilled into the panel for "Ride back in".
  bool has_last_ride() const { return has_last_ride_; }
  bool last_ride_hosted() const { return last_ride_host_; }
  std::uint64_t remote_applied() const { return remote_applied_; }
  std::uint64_t batches_sent() const { return sent_; }

  static constexpr std::uint16_t kDefaultPort = 24650;

 private:
  team::JoinOptions base_join() const;
  void send_presence(bool force);
  void start_reconnect();
  void tick_reconnect(double dt);
  void rode_back_in(const team::Docs& docs);
  void save_last_ride() const;
  void load_last_ride();
  std::string team_dir() const;

  std::unique_ptr<TeamBinding> binding_;
  ProjectInfo project_;
  team::TeamClient client_;
  std::unique_ptr<team::TeamServer> server_;
  std::vector<std::pair<std::string, team::Op>> held_;  // remote ops while busy
  bool loaded_ = false;
  float cursor_[3] = {0, 0, 0};
  bool cursor_valid_ = false;
  bool cursor_pinned_ = false;
  std::string last_presence_;
  double presence_clock_ = 0.0, heartbeat_clock_ = 0.0, resync_clock_ = 0.0;
  std::uint64_t resyncs_ = 0, remote_applied_ = 0, sent_ = 0;
  std::vector<std::string> notes_;
  std::deque<std::string> recent_notes_;
  void finish_asset_pass();
  void choose_files_backend();
  void tick_previews(double dt, bool busy);
  void forget_previews(const std::vector<team::Op>& ops);
  std::shared_ptr<vault::Backend> assets_;
  std::map<std::string, TeamLocalFile> files_;  // project-relative path -> last scan
  std::map<std::string, std::string> synced_;   // path -> sha the session confirmed here
  std::map<std::string, std::string> asset_sent_;    // path -> sha sent, not confirmed yet
  std::set<std::string> fetched_;               // assets that came from the session
  std::vector<std::string> arrived_;
  std::set<std::string> held_noted_;            // read-only: files we said stay here
  std::string files_url_, files_key_;           // typed in the Team panel (override)
  std::string files_route_, files_error_;
  bool files_chosen_ = false;
  std::shared_ptr<SessionBlobStore> blob_store_;  // hosting: files over the session
  double asset_clock_ = 0.0;
  bool first_asset_pass_ = true;  // joining: the session's files win
  std::uint64_t assets_up_ = 0, assets_down_ = 0, assets_removed_ = 0;
  // Mid-drag previews.
  double preview_clock_ = 0.0;
  std::string last_preview_;
  std::vector<std::uint64_t> my_preview_ids_;
  struct RemotePreview {
    std::uint32_t member = 0;
    double age = 0.0;
  };
  std::map<std::uint64_t, RemotePreview> remote_preview_;
  std::map<std::uint32_t, std::uint64_t> preview_closed_;  // member -> last ended preview n
  std::uint64_t previews_sent_ = 0, previews_shown_ = 0;
  // Riding back in.
  team::JoinOptions last_join_;
  bool last_join_host_ = false;
  bool was_joined_ = false;
  bool reconnecting_ = false;
  bool rejoin_pending_ = false;  // the next welcome is a ride back in
  team::SceneDoc rejoin_base_;   // the session as we last knew it
  double reconnect_clock_ = 0.0, reconnect_wait_ = 1.0;
  int reconnect_tries_ = 0;
  std::uint64_t rejoins_ = 0;
  std::string saved_rejoin_;     // token from last.json (editor restart)
  bool has_last_ride_ = false, last_ride_host_ = false;
  TeamAssetSync asset_sync_;  // last member: joined before the rest go
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
