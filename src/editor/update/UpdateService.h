#pragma once

// Background half of the update notifier (docs/updates.md): asks GitHub for
// the latest release and fetches + verifies its installer, on worker
// threads, through the Vault's HTTP client (vault/Http.h: WinHTTP for
// https:// on Windows, plain sockets for http://). The UI thread polls.
// Errors never throw or block; they come back as text for the log.

#include "editor/update/UpdateCheck.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace update {

struct ServiceOptions {
  std::string latest_url = kLatestReleaseUrl;
  std::string current_version = update::current_version();
  int timeout_ms = 10000;          // the API call; downloads use 60 s per read
  std::string download_dir;        // "" = <temp>/TombStoneUpdate
  bool dry_run = false;            // smoke: log instead of opening the browser / running the installer
};

// Can this build reach `url`? https:// needs WinHTTP (Windows); http:// works everywhere.
bool url_supported(const std::string& url);

class UpdateService {
 public:
  enum class Check { Idle, Running, Newer, UpToDate, Failed };
  enum class Download { Idle, Running, Ready, Failed };

  explicit UpdateService(ServiceOptions opt = {});
  ~UpdateService();  // cancels downloads; workers own their state, nothing blocks
  UpdateService(const UpdateService&) = delete;
  UpdateService& operator=(const UpdateService&) = delete;

  const ServiceOptions& options() const { return opt_; }

  // GET latest_url on a worker. No-op while one is running.
  void start_check();
  Check check_state() const;
  ReleaseInfo release() const;  // valid once check_state() is Newer / UpToDate
  // Seed the release (a cached result: tag + html_url, no assets; the
  // download re-asks the API for the assets).
  void set_release(const ReleaseInfo& r);

  // Fetch the installer (update.exe, else setup.exe) of release() into
  // download_dir, checked against SHA256SUMS.txt when the release has one.
  void start_download();
  Download download_state() const;
  double download_progress() const;   // 0..1, or -1 when the size is unknown
  std::string downloaded_path() const;  // valid when Ready
  std::string downloaded_asset() const;
  bool verified() const;               // a checksum matched (SUMS or GitHub digest)

  std::string last_error() const;
  // Smoke helper: wait until no worker runs (true) or timeout (false).
  bool wait_idle(int timeout_ms) const;

  struct Shared;

 private:
  ServiceOptions opt_;
  std::shared_ptr<Shared> s_;
};

// Hand the downloaded installer to the OS (ShellExecute: UAC prompts work).
bool launch_installer(const std::string& path, std::string* error);
// Open a web page in the default browser.
bool open_url(const std::string& url, std::string* error);

}  // namespace update
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
