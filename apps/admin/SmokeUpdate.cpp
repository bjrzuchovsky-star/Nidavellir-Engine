// The update notifier smoke (ts_admin --smoke-update-check, also part of
// --smoke). No internet: semver compare, the GitHub release JSON, asset
// selection (-update.exe before -setup.exe), SHA256SUMS.txt lookups and
// the 6-hour throttle on canned data; then the background check and the
// verified download against an in-process HTTP server on loopback (the
// Vault's client and server); then the Project Manager flow through
// AppFlow (banner, Skip, Later, the settings cache, TS_FAKE_UPDATE, Update
// in dry-run mode). See docs/updates.md.
#include "SmokeUpdate.h"

#include "editor/AppFlow.h"
#include "editor/screens/ProjectManagerScreen.h"
#include "editor/settings/Settings.h"
#include "editor/update/UpdateCheck.h"
#include "editor/update/UpdateService.h"
#include "vault/Http.h"
#include "vault/Sha256.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace up = ts::tombstone::editor::update;
namespace tv = ts::tombstone::vault;

namespace ts {
namespace tombstone {

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "[smoke-update] FAIL: " << what << '\n';
    ++g_failures;
  }
}

void put_env(const char* k, const std::string& v) {
#if defined(_WIN32)
  _putenv_s(k, v.c_str());  // "" removes it
#else
  if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
}

std::string env_or(const char* k) {
  const char* v = std::getenv(k);
  return v ? v : "";
}

std::string asset_json(const std::string& name, const std::string& url, std::size_t size,
                       const std::string& digest = {}) {
  std::string j = "{\"name\":\"" + name + "\",\"size\":" + std::to_string(size) +
                  ",\"content_type\":\"application/octet-stream\",\"browser_download_url\":\"" + url + "\"";
  if (!digest.empty()) j += ",\"digest\":\"sha256:" + digest + "\"";
  return j + "}";
}

std::string release_json(const std::string& tag, const std::string& assets) {
  return "{\"url\":\"https://api.github.com/repos/bjrzuchovsky-star/TombStone_Engine/releases/1\","
         "\"html_url\":\"https://github.com/bjrzuchovsky-star/TombStone_Engine/releases/tag/" + tag + "\","
         "\"id\":1,\"tag_name\":\"" + tag + "\",\"name\":\"TombStone Engine " + tag + "\","
         "\"draft\":false,\"prerelease\":false,\"published_at\":\"2026-11-01T00:00:00Z\","
         "\"assets\":[" + assets + "],\"body\":\"Notes with \\\"quotes\\\" and \\u00e9.\"}";
}

void pure_checks() {
  using up::is_newer;
  check(is_newer("v0.1.1", "0.1.0"), "v0.1.1 > 0.1.0");
  check(is_newer("0.2.0", "0.1.9"), "0.2.0 > 0.1.9");
  check(is_newer("v0.10.0", "0.9.9"), "0.10.0 > 0.9.9 (numeric, not text)");
  check(is_newer("V1.0", "0.9.9"), "V1.0 (two parts) > 0.9.9");
  check(!is_newer("v0.1.0", "0.1.0"), "same version is not newer");
  check(!is_newer("v0.0.9", "0.1.0"), "older is not newer");
  check(!is_newer("1.0.0-beta.1", "1.0.0"), "pre-release sorts before its release");
  check(is_newer("1.0.0", "1.0.0-rc.1"), "release beats its rc");
  check(is_newer("1.0.0-rc.2", "1.0.0-rc.1"), "rc.2 > rc.1");
  check(is_newer("1.0.0-rc.10", "1.0.0-rc.9"), "rc.10 > rc.9 (numeric identifiers)");
  check(is_newer("1.0.0-beta", "1.0.0-alpha"), "beta > alpha");
  check(!is_newer("0.1.0+build.7", "0.1.0"), "build metadata is ignored");
  check(!is_newer("latest", "0.1.0") && !is_newer("v1.x.0", "0.1.0") && !is_newer("", "0.1.0") &&
            !is_newer("1.2.3.4", "0.1.0") && !is_newer("1.", "0.1.0"),
        "garbage tags never count as newer");
  check(up::strip_v("v0.2.0") == "0.2.0" && up::strip_v("0.2.0") == "0.2.0", "strip_v");
  up::SemVer sv;
  check(up::parse_semver(up::current_version(), &sv), std::string("compiled-in version parses: ") + up::current_version());

  // Canned GitHub JSON: both installers, the zip and SHA256SUMS.txt.
  const std::string both = release_json(
      "v0.2.0", asset_json("TombStone-0.2.0-win64.zip", "https://x/zip", 10) + "," +
                    asset_json("TombStone-0.2.0-setup.exe", "https://x/setup", 20) + "," +
                    asset_json("TombStone-0.2.0-update.exe", "https://x/update", 30,
                               std::string(64, 'a')) + "," +
                    asset_json("SHA256SUMS.txt", "https://x/sums", 40));
  up::ReleaseInfo r;
  std::string err;
  check(up::parse_release_json(both, &r, &err), "parse release JSON: " + err);
  check(r.tag == "v0.2.0" && r.version == "0.2.0" && r.assets.size() == 4, "release fields");
  check(r.html_url == "https://github.com/bjrzuchovsky-star/TombStone_Engine/releases/tag/v0.2.0", "html_url");
  up::AssetKind kind = up::AssetKind::None;
  const up::ReleaseAsset* a = up::select_update_asset(r, &kind);
  check(a && a->name == "TombStone-0.2.0-update.exe" && kind == up::AssetKind::Update, "prefers -update.exe");
  check(a && a->sha256 == std::string(64, 'a') && a->size == 30, "asset digest + size");
  check(up::find_sums_asset(r) && up::find_sums_asset(r)->url == "https://x/sums", "finds SHA256SUMS.txt");

  const std::string setup_only = release_json(
      "v0.2.0", asset_json("TombStone-0.2.0-setup.exe", "https://x/setup", 20) + "," +
                    asset_json("TombStone-0.1.0-update.exe", "https://x/old-update", 5));
  check(up::parse_release_json(setup_only, &r, &err), "parse setup-only JSON");
  a = up::select_update_asset(r, &kind);
  check(a && a->name == "TombStone-0.2.0-setup.exe" && kind == up::AssetKind::Setup,
        "falls back to -setup.exe (and ignores another version's update.exe)");
  check(!up::find_sums_asset(r), "no SHA256SUMS.txt");

  const std::string none = release_json("v0.2.0", asset_json("TombStone-0.2.0-win64.zip", "https://x/zip", 10));
  check(up::parse_release_json(none, &r, &err), "parse zip-only JSON");
  check(!up::select_update_asset(r, &kind) && kind == up::AssetKind::None, "no installer -> nothing selected");
  check(!up::parse_release_json("{\"message\":\"API rate limit exceeded\"}", &r, &err), "rate-limit body rejected");
  check(!up::parse_release_json("<html>", &r, &err), "non-JSON rejected");

  const std::string h1 = std::string(64, '1'), h2 = std::string(64, 'B');
  const std::string sums = h1 + "  TombStone-0.2.0-setup.exe\r\n" + h2 + " *TombStone-0.2.0-update.exe\n" +
                           "nothex  TombStone-0.2.0-win64.zip\n";
  check(up::sums_lookup(sums, "TombStone-0.2.0-setup.exe") == h1, "sums: text mode line (CRLF)");
  check(up::sums_lookup(sums, "TombStone-0.2.0-update.exe") == std::string(64, 'b'), "sums: binary marker, lowercased");
  check(up::sums_lookup(sums, "TombStone-0.2.0-win64.zip").empty(), "sums: bad hash ignored");
  check(up::sums_lookup(sums, "other.exe").empty(), "sums: missing file");

  const std::int64_t now = 1'800'000'000;
  check(up::check_due(0, now), "never checked -> due");
  check(!up::check_due(now - 60, now), "a minute ago -> not due");
  check(!up::check_due(now - up::kCheckIntervalSeconds + 1, now), "just under 6 h -> not due");
  check(up::check_due(now - up::kCheckIntervalSeconds, now), "6 h -> due");
  check(up::check_due(now + 3600, now), "clock moved back -> due");
  check(up::should_notify("v0.2.0", "0.1.0", ""), "notify newer");
  check(!up::should_notify("v0.2.0", "0.1.0", "0.2.0"), "skipped version stays quiet");
  check(up::should_notify("v0.3.0", "0.1.0", "0.2.0"), "a newer one than the skipped shows again");
  check(!up::should_notify("v0.1.0", "0.1.0", ""), "same version quiet");

  const std::string saved = env_or("TS_FAKE_UPDATE");
  put_env("TS_FAKE_UPDATE", "9.9.9");
  check(up::fake_update_version() == "9.9.9", "TS_FAKE_UPDATE=9.9.9");
  put_env("TS_FAKE_UPDATE", "v9.9.9");
  check(up::fake_update_version() == "9.9.9", "TS_FAKE_UPDATE=v9.9.9");
  put_env("TS_FAKE_UPDATE", "nope");
  check(up::fake_update_version().empty(), "TS_FAKE_UPDATE=nope ignored");
  put_env("TS_FAKE_UPDATE", saved);
}

// Loopback "GitHub": /good, /bad (wrong checksum), /setup (setup.exe +
// digest, no SUMS), /none (zip only), /limit (403 rate limit).
struct FakeGitHub {
  tv::HttpServer server;
  std::string base;
  std::string payload;
  std::atomic<int> api_calls{0};

  bool start(std::string* err) {
    payload.assign(300000, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>((i * 31 + 7) & 0xff);
    tv::ServerOptions so;
    so.port = 0;
    return server.start(so, [this](const tv::HttpRequest& rq, tv::HttpResponse* rs) { handle(rq, rs); }, err) &&
           (base = "http://127.0.0.1:" + std::to_string(server.port()), true);
  }

  void handle(const tv::HttpRequest& rq, tv::HttpResponse* rs) {
    const std::string sha = tv::sha256_hex(payload);
    const std::string& p = rq.path;
    auto rel = [&](const std::string& tag, const std::string& assets) {
      ++api_calls;
      if (rq.header("user-agent").empty()) {
        rs->status = 403;
        rs->body = "{\"message\":\"no user agent\"}";
        return;
      }
      rs->body = release_json(tag, assets);
    };
    const std::string up_name = "TombStone-9.9.9-update.exe";
    if (p == "/good/releases/latest") {
      rel("v9.9.9", asset_json(up_name, base + "/dl/" + up_name, payload.size()) + "," +
                        asset_json("TombStone-9.9.9-setup.exe", base + "/dl/setup", payload.size()) + "," +
                        asset_json("SHA256SUMS.txt", base + "/dl/SHA256SUMS.txt", 100));
    } else if (p == "/bad/releases/latest") {
      rel("v9.9.9", asset_json(up_name, base + "/dl/" + up_name, payload.size()) + "," +
                        asset_json("SHA256SUMS.txt", base + "/dl/BADSUMS.txt", 100));
    } else if (p == "/setup/releases/latest") {
      rel("v9.9.9", asset_json("TombStone-9.9.9-setup.exe", base + "/dl/" + up_name, payload.size(), sha));
    } else if (p == "/none/releases/latest") {
      rel("v9.9.9", asset_json("TombStone-9.9.9-win64.zip", base + "/dl/zip", 3));
    } else if (p == "/same/releases/latest") {
      rel(std::string("v") + up::current_version(), "");
    } else if (p == "/limit/releases/latest") {
      ++api_calls;
      rs->status = 403;
      rs->headers["X-RateLimit-Remaining"] = "0";
      rs->body = "{\"message\":\"API rate limit exceeded\"}";
    } else if (p == "/dl/" + up_name || p == "/dl/setup") {
      rs->content_type = "application/octet-stream";
      rs->body = payload;
    } else if (p == "/dl/SHA256SUMS.txt") {
      rs->content_type = "text/plain";
      rs->body = sha + "  " + up_name + "\n" + std::string(64, 'c') + "  TombStone-9.9.9-setup.exe\n";
    } else if (p == "/dl/BADSUMS.txt") {
      rs->content_type = "text/plain";
      rs->body = std::string(64, 'd') + "  " + up_name + "\n";
    } else {
      rs->status = 404;
      rs->body = "{}";
    }
  }
};

up::ServiceOptions opts(const FakeGitHub& gh, const std::string& route, const fs::path& dl) {
  up::ServiceOptions o;
  o.latest_url = gh.base + "/" + route + "/releases/latest";
  o.current_version = up::current_version();
  o.timeout_ms = 5000;
  o.download_dir = dl.string();
  o.dry_run = true;
  return o;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void service_checks(FakeGitHub& gh, const fs::path& root) {
  using S = up::UpdateService;
  {
    S svc(opts(gh, "good", root / "dl-good"));
    svc.start_check();
    check(svc.wait_idle(10000), "check finishes");
    check(svc.check_state() == S::Check::Newer, "loopback check finds v9.9.9: " + svc.last_error());
    check(svc.release().assets.size() == 3, "release assets over the wire");
    svc.start_download();
    check(svc.wait_idle(20000), "download finishes");
    check(svc.download_state() == S::Download::Ready, "download ready: " + svc.last_error());
    check(svc.verified(), "verified against SHA256SUMS.txt");
    check(svc.downloaded_asset() == "TombStone-9.9.9-update.exe", "downloaded the -update.exe");
    check(slurp(svc.downloaded_path()) == gh.payload, "downloaded bytes match");
    check(!fs::exists(root / "dl-good" / "TombStone-9.9.9-update.exe.part"), "no .part left behind");
    check(svc.download_progress() >= 0.999, "progress reaches 100%");
  }
  {
    S svc(opts(gh, "bad", root / "dl-bad"));
    svc.start_check();
    svc.wait_idle(10000);
    svc.start_download();
    check(svc.wait_idle(20000), "bad download finishes");
    check(svc.download_state() == S::Download::Failed &&
              svc.last_error().find("checksum mismatch") != std::string::npos,
          "checksum mismatch refused: " + svc.last_error());
    check(!fs::exists(root / "dl-bad" / "TombStone-9.9.9-update.exe"), "mismatched file deleted");
  }
  {
    S svc(opts(gh, "setup", root / "dl-setup"));
    svc.start_check();
    svc.wait_idle(10000);
    svc.start_download();
    svc.wait_idle(20000);
    check(svc.download_state() == S::Download::Ready && svc.verified() &&
              svc.downloaded_asset() == "TombStone-9.9.9-setup.exe",
          "setup.exe fallback verified by the asset digest: " + svc.last_error());
  }
  {
    S svc(opts(gh, "none", root / "dl-none"));
    svc.start_check();
    svc.wait_idle(10000);
    svc.start_download();
    svc.wait_idle(20000);
    check(svc.download_state() == S::Download::Failed, "no installer -> download fails cleanly");
  }
  {
    S svc(opts(gh, "limit", root / "dl-limit"));
    svc.start_check();
    svc.wait_idle(10000);
    check(svc.check_state() == S::Check::Failed && svc.last_error().find("rate limited") != std::string::npos,
          "rate limit -> quiet failure: " + svc.last_error());
  }
  {
    S svc(opts(gh, "same", root / "dl-same"));
    svc.start_check();
    svc.wait_idle(10000);
    check(svc.check_state() == S::Check::UpToDate, "same version -> up to date");
  }
  {
    up::ServiceOptions o = opts(gh, "good", root / "dl-x");
    o.latest_url = "http://127.0.0.1:1/nothing";  // nobody listens on port 1
    o.timeout_ms = 2000;
    S svc(o);
    svc.start_check();
    check(svc.wait_idle(15000), "offline check returns");
    check(svc.check_state() == S::Check::Failed, "offline -> quiet failure");
  }
#if !defined(_WIN32)
  check(!up::url_supported(up::kLatestReleaseUrl), "https disabled off Windows (Vault client is http-only here)");
#else
  check(up::url_supported(up::kLatestReleaseUrl), "https supported on Windows (WinHTTP)");
#endif
}

// Loading -> Login -> Project Manager on a headless AppFlow.
bool to_project_manager(ed::AppFlow& flow) {
  flow.start();
  for (int i = 0; i < 400 && flow.current_state() == ed::AppState::Loading; ++i) flow.tick(1.0f / 60.0f);
  return flow.submit_dev_login() && flow.current_state() == ed::AppState::ProjectManager;
}

bool pump_until(ed::AppFlow& flow, const std::function<bool()>& done, int ms = 10000) {
  for (int waited = 0; waited < ms; waited += 10) {
    flow.pump_updates();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

void flow_checks(FakeGitHub& gh, const fs::path& root) {
  const fs::path cwd = fs::current_path();
  const fs::path work = root / "flow";
  fs::create_directories(work);
  fs::current_path(work);
  {
    ed::Settings seed = ed::SettingsStore::make_defaults();
    seed.username = "dev";
    ed::SettingsStore::save(seed);
  }
  const up::ServiceOptions o = opts(gh, "good", root / "dl-flow");

  // 1. First open: a real (loopback) check, the notice, then Skip.
  {
    gh.api_calls = 0;
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    check(to_project_manager(flow), "flow reaches the Project Manager");
    check(pump_until(flow, [&] { return flow.update_notice_visible(); }), "banner shows for v9.9.9");
    check(flow.update_offer_version() == "9.9.9" && gh.api_calls == 1, "offer v9.9.9 after one API call");
    const ed::Settings s = ed::SettingsStore::load();
    check(!s.update_last_check.empty() && s.update_cached_version == "9.9.9", "check time + result cached in settings.json");
    flow.request_quit();
  }
  // 2. Within 6 h: no API call; cached result shows the notice; Skip persists.
  {
    gh.api_calls = 0;
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    to_project_manager(flow);
    flow.pump_updates();
    check(gh.api_calls == 0, "no second API call inside 6 hours");
    check(flow.update_notice_visible(), "cached result still offers v9.9.9");
    flow.tick(0.0f);
    // Press "Skip this version" through the screen, as the button does.
    flow.press_update_action_for_smoke(ed::UpdateAction::Skip);
    check(!flow.update_notice_visible(), "Skip hides the notice");
    check(ed::SettingsStore::load().update_skip_version == "9.9.9", "Skip is saved");
    flow.request_quit();
  }
  // 3. Skipped version stays quiet; a forced re-check (old timestamp) too.
  {
    ed::Settings s = ed::SettingsStore::load();
    s.update_last_check = "1";
    ed::SettingsStore::save(s);
    gh.api_calls = 0;
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    to_project_manager(flow);
    flow.update_service()->wait_idle(5000);
    flow.pump_updates();
    check(gh.api_calls == 1 && !flow.update_notice_visible(), "skipped v9.9.9 stays quiet after a re-check");
    flow.request_quit();
  }
  // 4. Toggle off: nothing asked, nothing shown.
  {
    ed::Settings s = ed::SettingsStore::load();
    s.update_skip_version.clear();
    s.update_last_check = "1";
    s.check_updates_on_start = false;
    ed::SettingsStore::save(s);
    gh.api_calls = 0;
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    to_project_manager(flow);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    flow.pump_updates();
    check(gh.api_calls == 0 && !flow.update_notice_visible(), "'Check for updates on start' off -> no check");
    check(!ed::SettingsStore::load().check_updates_on_start, "toggle persists");
    flow.request_quit();
  }
  // 5. Update: download + verify, then (dry run) launch and close the editor.
  {
    ed::Settings s = ed::SettingsStore::load();
    s.check_updates_on_start = true;
    ed::SettingsStore::save(s);
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    to_project_manager(flow);
    check(pump_until(flow, [&] { return flow.update_notice_visible(); }), "notice before Update");
    flow.press_update_action_for_smoke(ed::UpdateAction::Later);
    check(!flow.update_notice_visible(), "Later hides the notice");
    check(ed::SettingsStore::load().update_skip_version.empty(), "Later does not skip");
    flow.request_quit();
  }
  {
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    to_project_manager(flow);
    check(flow.update_notice_visible(), "notice back next session after Later");
    flow.press_update_action_for_smoke(ed::UpdateAction::Update);
    check(pump_until(flow, [&] { return !flow.is_running(); }, 20000), "Update downloads, 'launches' and closes the editor");
    check(fs::exists(root / "dl-flow" / "TombStone-9.9.9-update.exe"), "installer saved to the download folder");
  }
  // 6. TS_FAKE_UPDATE forces the notice without asking anyone.
  {
    const std::string saved = env_or("TS_FAKE_UPDATE");
    put_env("TS_FAKE_UPDATE", "9.9.9");
    ed::Settings s = ed::SettingsStore::load();
    s.check_updates_on_start = false;
    ed::SettingsStore::save(s);
    gh.api_calls = 0;
    ed::AppFlow flow;
    flow.enable_update_checks(&o);
    to_project_manager(flow);
    flow.pump_updates();
    check(flow.update_notice_visible() && flow.update_offer_version() == "9.9.9" && gh.api_calls == 0,
          "TS_FAKE_UPDATE=9.9.9 shows the notice, no network");
    flow.press_update_action_for_smoke(ed::UpdateAction::Update);
    flow.pump_updates();
    check(flow.is_running(), "Update on the test notice downloads nothing");
    flow.press_update_action_for_smoke(ed::UpdateAction::Later);
    check(!flow.update_notice_visible(), "Later hides the test notice");
    flow.request_quit();
    put_env("TS_FAKE_UPDATE", saved);
  }
  // 7. Without enable_update_checks (every other smoke): never a check.
  {
    gh.api_calls = 0;
    ed::AppFlow flow;
    to_project_manager(flow);
    flow.pump_updates();
    check(gh.api_calls == 0 && !flow.update_notice_visible(), "update checks are off unless the GUI enables them");
    flow.request_quit();
  }
  fs::current_path(cwd);
}

}  // namespace

int run_update_smoke() {
  g_failures = 0;
  std::cout << "[smoke-update] TombStone " << up::current_version() << " update notifier\n";
  const std::string fake = env_or("TS_FAKE_UPDATE");
  const std::string no_check = env_or("TS_NO_UPDATE_CHECK");
  put_env("TS_FAKE_UPDATE", "");
  put_env("TS_NO_UPDATE_CHECK", "");
  pure_checks();
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "ts_update_smoke";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  FakeGitHub gh;
  std::string err;
  if (!gh.start(&err)) {
    check(false, "loopback server: " + err);
  } else {
    service_checks(gh, root);
    flow_checks(gh, root);
    gh.server.stop();
  }
  put_env("TS_FAKE_UPDATE", fake);
  put_env("TS_NO_UPDATE_CHECK", no_check);
  fs::remove_all(root, ec);
  if (g_failures) {
    std::cerr << "[smoke-update] FAILED (" << g_failures << ")\n";
    return 1;
  }
  std::cout << "[smoke-update] OK (semver, release JSON, asset choice, SHA256SUMS, throttle, loopback check + "
               "verified download, banner / Skip / Later / Update, TS_FAKE_UPDATE)\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
