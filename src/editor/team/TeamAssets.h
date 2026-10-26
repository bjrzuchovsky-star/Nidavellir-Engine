#pragma once

// Live Team Claims: project files through the Vault, off the main thread
// (docs/team-claims.md, 4). One pass = scan assets/ (hash what changed),
// send new / changed files up, fetch the session's newer files, and carry
// deletes both ways. The pass runs on a worker thread; the editor's frame
// only starts it, reads its progress and takes its result.

#include "team/TeamDoc.h"
#include "vault/VaultBackend.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

struct TeamLocalFile {
  long long mtime = 0;
  std::uint64_t size = 0;
  std::string sha;
};

// What the Team panel shows while freight is moving.
struct TeamAssetProgress {
  bool running = false;
  std::string stage;        // "Scanning", "Sending", "Fetching"
  std::string current;      // project-relative path
  int files_done = 0, files_total = 0;
  std::uint64_t bytes_done = 0, bytes_total = 0;
};

// The files a pass carries: everything under assets/, and each asset's
// .meta in the metadata folder (.tombstone/meta/assets/...), so GUIDs and
// import settings match on every PC.
bool team_file_rel(const std::string& rel);
bool team_meta_rel(const std::string& rel);          // ".tombstone/meta/assets/....meta"
std::string team_meta_asset(const std::string& rel);  // its asset ("assets/...") or ""

struct TeamAssetPass {
  // In (copies: the pass never touches the session).
  std::string root;                                   // claim folder
  std::map<std::string, team::AssetRef> session;      // confirmed assets
  std::map<std::string, TeamLocalFile> files;         // last scan
  std::map<std::string, std::string> synced;          // path -> sha the session confirmed here
  std::map<std::string, std::string> sent;            // path -> sha sent, not confirmed yet
  std::set<std::string> fetched;                      // assets that came from the session (not authored here)
  bool first = false;                                 // joining: the session's files win
  bool can_write = true;
  // Out.
  std::vector<team::Op> ops;  // asset ops to commit (sha "" = deleted here)
  std::vector<std::string> notes;
  std::vector<std::string> arrived;  // project paths written / removed by this pass (caches reload)
  std::vector<std::string> held_back;  // read-only: local files not sent
  std::string error;                 // the Vault / host could not be used ("" = fine)
  std::uint64_t up = 0, down = 0, removed = 0;
};

class TeamAssetSync {
 public:
  TeamAssetSync() = default;
  ~TeamAssetSync() { wait(); }
  TeamAssetSync(const TeamAssetSync&) = delete;
  TeamAssetSync& operator=(const TeamAssetSync&) = delete;

  // Start a pass on the worker (false if one is still running).
  bool start(TeamAssetPass pass, std::shared_ptr<vault::Backend> backend);
  bool running() const;
  // The finished pass (once), else false. Never blocks.
  bool take(TeamAssetPass* out);
  // Ask the running pass to stop after the current file and wait for it.
  void cancel_and_wait();
  void wait();
  TeamAssetProgress progress() const;

  // The pass itself (runs on the worker; public for tests).
  static void run(TeamAssetPass& p, vault::Backend& backend, TeamAssetSync* self);

 private:
  void set_progress(const TeamAssetProgress& p);
  std::future<TeamAssetPass> job_;
  mutable std::mutex mu_;
  TeamAssetProgress progress_;
  std::atomic<bool> cancel_{false};
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
