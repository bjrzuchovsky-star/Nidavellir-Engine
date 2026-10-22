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
#include "team/TeamClient.h"
#include "team/TeamServer.h"

#include <cstdint>
#include <functional>
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
};

std::unique_ptr<TeamBinding> make_team_binding(Editor2DScreen& screen);
std::unique_ptr<TeamBinding> make_team_binding(Editor3DScreen& screen);

struct TeamHostSettings {
  std::uint16_t port = 24650;  // UDP
  std::string pass;            // optional passcode
  std::string user;            // display name
};

struct TeamJoinSettings {
  std::string host = "127.0.0.1";
  std::uint16_t port = 24650;
  std::string key;   // Vault API key (ts_vault --team)
  std::string pass;  // passcode (editor-hosted)
  std::string user;
  std::string claim = "*";  // "*" = whatever the host is running
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
  int mode = 0;  // 0 = join, 1 = host
  std::string last_error;
};

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
  // Another member holding entity `id` (0 = free or ours).
  std::uint32_t lock_holder(std::uint64_t id) const;
  const team::RemoteMember* holder_member(std::uint64_t id) const;

  std::vector<std::string> take_notes();
  std::string status_line() const;
  std::uint64_t resyncs() const { return resyncs_; }
  std::uint64_t remote_applied() const { return remote_applied_; }
  std::uint64_t batches_sent() const { return sent_; }

  static constexpr std::uint16_t kDefaultPort = 24650;

 private:
  team::JoinOptions base_join() const;
  void send_presence(bool force);

  std::unique_ptr<TeamBinding> binding_;
  ProjectInfo project_;
  team::TeamClient client_;
  std::unique_ptr<team::TeamServer> server_;
  std::vector<std::pair<std::string, team::Op>> held_;  // remote ops while busy
  bool loaded_ = false;
  float cursor_[3] = {0, 0, 0};
  bool cursor_valid_ = false;
  std::string last_presence_;
  double presence_clock_ = 0.0, heartbeat_clock_ = 0.0, resync_clock_ = 0.0;
  std::uint64_t resyncs_ = 0, remote_applied_ = 0, sent_ = 0;
  std::vector<std::string> notes_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
