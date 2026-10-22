#include "editor/update/UpdateService.h"

#include "vault/Http.h"
#include "vault/Sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ts {
namespace tombstone {
namespace editor {
namespace update {

namespace fs = std::filesystem;

struct UpdateService::Shared {
  mutable std::mutex mu;
  mutable std::condition_variable cv;
  UpdateService::Check check = UpdateService::Check::Idle;
  UpdateService::Download download = UpdateService::Download::Idle;
  ReleaseInfo release;
  std::string error;
  std::string path, asset;
  bool verified = false;
  int workers = 0;
  std::atomic<bool> cancel{false};
  std::atomic<std::uint64_t> got{0};
  std::atomic<std::uint64_t> total{0};
};

namespace {

vault::Headers api_headers(const ServiceOptions& o) {
  return {{"User-Agent", std::string("TombStone-Engine/") + o.current_version + " (update-check)"},
          {"Accept", "application/vnd.github+json"},
          {"X-GitHub-Api-Version", "2022-11-28"}};
}

vault::Headers dl_headers(const ServiceOptions& o) {
  return {{"User-Agent", std::string("TombStone-Engine/") + o.current_version + " (updater)"},
          {"Accept", "application/octet-stream"}};
}

std::string describe_status(const vault::ClientResult& r) {
  if (r.status == 403 || r.status == 429) {
    const auto it = r.headers.find("x-ratelimit-remaining");
    if (r.status == 429 || (it != r.headers.end() && it->second == "0")) return "rate limited (HTTP " + std::to_string(r.status) + ")";
  }
  return "HTTP " + std::to_string(r.status);
}

// GET latest_url -> ReleaseInfo. False + error on any failure.
bool fetch_release(const ServiceOptions& o, ReleaseInfo* out, std::string* error) {
  if (!url_supported(o.latest_url)) {
    *error = "https needs the Windows editor; update checks are off on this platform";
    return false;
  }
  vault::ClientOptions copt;
  copt.timeout_ms = o.timeout_ms;
  copt.max_body = 4u * 1024u * 1024u;
  vault::ClientResult r;
  std::string err;
  if (!vault::http_request("GET", o.latest_url, api_headers(o), {}, &r, &err, copt)) {
    *error = err.empty() ? "no response" : err;
    return false;
  }
  if (r.status != 200) {
    *error = describe_status(r);
    return false;
  }
  return parse_release_json(r.body, out, error);
}

class WorkerGuard {
 public:
  explicit WorkerGuard(std::shared_ptr<UpdateService::Shared> s) : s_(std::move(s)) {}
  ~WorkerGuard() {
    std::lock_guard<std::mutex> lk(s_->mu);
    --s_->workers;
    s_->cv.notify_all();
  }
 private:
  std::shared_ptr<UpdateService::Shared> s_;
};

}  // namespace

bool url_supported(const std::string& url) {
  vault::UrlParts u;
  if (!vault::split_url(url, &u)) return false;
#if defined(_WIN32)
  return true;
#else
  return u.scheme == "http";
#endif
}

UpdateService::UpdateService(ServiceOptions opt) : opt_(std::move(opt)), s_(std::make_shared<Shared>()) {
  if (opt_.download_dir.empty()) {
    std::error_code ec;
    opt_.download_dir = (fs::temp_directory_path(ec) / "TombStoneUpdate").string();
  }
}

UpdateService::~UpdateService() { s_->cancel = true; }

void UpdateService::start_check() {
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    if (s_->check == Check::Running) return;
    s_->check = Check::Running;
    s_->error.clear();
    ++s_->workers;
  }
  std::thread([s = s_, o = opt_] {
    WorkerGuard guard(s);
    ReleaseInfo r;
    std::string err;
    const bool ok = fetch_release(o, &r, &err);
    std::lock_guard<std::mutex> lk(s->mu);
    if (!ok) {
      s->check = Check::Failed;
      s->error = err;
      return;
    }
    s->release = r;
    s->check = is_newer(r.tag, o.current_version) ? Check::Newer : Check::UpToDate;
  }).detach();
}

UpdateService::Check UpdateService::check_state() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->check;
}

ReleaseInfo UpdateService::release() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->release;
}

void UpdateService::set_release(const ReleaseInfo& r) {
  std::lock_guard<std::mutex> lk(s_->mu);
  s_->release = r;
  if (s_->check != Check::Running) {
    s_->check = is_newer(r.tag, opt_.current_version) ? Check::Newer : Check::UpToDate;
  }
}

void UpdateService::start_download() {
  ReleaseInfo seed;
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    if (s_->download == Download::Running) return;
    s_->download = Download::Running;
    s_->error.clear();
    s_->verified = false;
    s_->path.clear();
    s_->asset.clear();
    s_->got = 0;
    s_->total = 0;
    seed = s_->release;
    ++s_->workers;
  }
  std::thread([s = s_, o = opt_, seed] {
    WorkerGuard guard(s);
    auto fail = [&](const std::string& why) {
      std::lock_guard<std::mutex> lk(s->mu);
      s->download = Download::Failed;
      s->error = why;
    };
    ReleaseInfo rel = seed;
    std::string err;
    if (rel.assets.empty()) {
      // A cached result carries no asset list: ask the API again.
      ReleaseInfo fresh;
      if (!fetch_release(o, &fresh, &err)) return fail("could not reach the release: " + err);
      if (!seed.tag.empty() && strip_v(fresh.tag) != strip_v(seed.tag) && !is_newer(fresh.tag, seed.tag)) {
        return fail("the latest release is now " + fresh.tag);
      }
      rel = fresh;
      std::lock_guard<std::mutex> lk(s->mu);
      s->release = rel;
    }
    AssetKind kind = AssetKind::None;
    const ReleaseAsset* asset = select_update_asset(rel, &kind);
    if (!asset) return fail("no TombStone-" + strip_v(rel.tag) + "-update.exe or -setup.exe on " + rel.tag);
    for (char c : asset->name) {
      if (c == '/' || c == '\\' || c == ':') return fail("odd asset name " + asset->name);
    }
    if (!url_supported(asset->url)) return fail("cannot download " + asset->url + " on this platform");

    // Expected checksum: SHA256SUMS.txt when attached (must list the file),
    // else GitHub's own asset digest when it gives one.
    std::string expected;
    if (const ReleaseAsset* sums = find_sums_asset(rel)) {
      vault::ClientOptions copt;
      copt.timeout_ms = o.timeout_ms;
      copt.max_body = 1024u * 1024u;
      vault::ClientResult r;
      if (!vault::http_request("GET", sums->url, dl_headers(o), {}, &r, &err, copt)) {
        return fail("could not fetch SHA256SUMS.txt: " + err);
      }
      if (r.status != 200) return fail("could not fetch SHA256SUMS.txt: " + describe_status(r));
      expected = sums_lookup(r.body, asset->name);
      if (expected.empty()) return fail(asset->name + " is not listed in SHA256SUMS.txt");
    } else if (!asset->sha256.empty()) {
      expected = asset->sha256;
    }

    std::error_code ec;
    fs::create_directories(o.download_dir, ec);
    const fs::path final_path = fs::path(o.download_dir) / asset->name;
    const fs::path part = fs::path(o.download_dir) / (asset->name + ".part");
    fs::remove(part, ec);
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write " + part.string());
    s->total = asset->size;
    vault::Sha256 sha;
    vault::ClientOptions copt;
    copt.timeout_ms = 60000;
    copt.sink = [&](const char* d, std::size_t n) {
      if (s->cancel) return false;
      out.write(d, static_cast<std::streamsize>(n));
      sha.update(d, n);
      s->got += n;
      return static_cast<bool>(out);
    };
    vault::ClientResult r;
    const bool ok = vault::http_request("GET", asset->url, dl_headers(o), {}, &r, &err, copt);
    out.close();
    if (!ok || r.status != 200) {
      fs::remove(part, ec);
      return fail("download failed: " + (ok ? describe_status(r) : err));
    }
    const std::uint64_t got = s->got;
    if (asset->size && got != asset->size) {
      fs::remove(part, ec);
      return fail("download was cut short (" + std::to_string(got) + " of " + std::to_string(asset->size) + " bytes)");
    }
    const std::string hex = sha.hex();
    if (!expected.empty() && hex != expected) {
      fs::remove(part, ec);
      return fail("checksum mismatch for " + asset->name + " (got " + hex.substr(0, 12) + "..., want " +
                  expected.substr(0, 12) + "...)");
    }
    fs::remove(final_path, ec);
    fs::rename(part, final_path, ec);
    if (ec) return fail("cannot move the download into place: " + ec.message());
    std::lock_guard<std::mutex> lk(s->mu);
    s->download = Download::Ready;
    s->path = final_path.string();
    s->asset = asset->name;
    s->verified = !expected.empty();
  }).detach();
}

UpdateService::Download UpdateService::download_state() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->download;
}

double UpdateService::download_progress() const {
  const std::uint64_t total = s_->total;
  if (!total) return -1.0;
  return std::min(1.0, static_cast<double>(s_->got.load()) / static_cast<double>(total));
}

std::string UpdateService::downloaded_path() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->path;
}

std::string UpdateService::downloaded_asset() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->asset;
}

bool UpdateService::verified() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->verified;
}

std::string UpdateService::last_error() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->error;
}

bool UpdateService::wait_idle(int timeout_ms) const {
  std::unique_lock<std::mutex> lk(s_->mu);
  return s_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return s_->workers == 0; });
}

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

bool shell_open(const std::wstring& target, std::string* error) {
  const HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    if (error) *error = "ShellExecute failed (code " + std::to_string(static_cast<long long>(reinterpret_cast<INT_PTR>(r))) + ")";
    return false;
  }
  return true;
}
}  // namespace

bool launch_installer(const std::string& path, std::string* error) {
  std::error_code ec;
  if (!fs::exists(fs::path(widen(path)), ec)) {
    if (error) *error = "installer missing: " + path;
    return false;
  }
  return shell_open(fs::path(widen(path)).wstring(), error);
}

bool open_url(const std::string& url, std::string* error) {
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
    if (error) *error = "not a web address: " + url;
    return false;
  }
  return shell_open(widen(url), error);
}
#else
namespace {
bool spawn_detached(const char* tool, const std::string& arg, std::string* error) {
  const pid_t child = fork();
  if (child < 0) {
    if (error) *error = "fork failed";
    return false;
  }
  if (child == 0) {
    if (fork() == 0) {
      const char* argv[] = {tool, arg.c_str(), nullptr};
      execvp(tool, const_cast<char* const*>(argv));
      _exit(127);
    }
    _exit(0);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  return true;
}
}  // namespace

bool launch_installer(const std::string& path, std::string* error) {
  (void)path;
  if (error) *error = "the installer only runs on Windows";
  return false;
}

bool open_url(const std::string& url, std::string* error) {
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
    if (error) *error = "not a web address: " + url;
    return false;
  }
#if defined(__APPLE__)
  return spawn_detached("open", url, error);
#else
  return spawn_detached("xdg-open", url, error);
#endif
}
#endif

}  // namespace update
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
