#include "SmokeTradingPost.h"

#include "editor/plugins/EditorPlugins.h"
#include "editor/projects/ProjectStore.h"
#include "editor/store/TradingPostPanel.h"
#include "plugin/DynamicLibrary.h"
#include "plugins/Builtins.h"
#include "store/Crypto.h"
#include "store/Fetch.h"
#include "store/Json.h"
#include "store/TradingPost.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
#define TS_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define TS_CLOSESOCK close
#endif

namespace fs = std::filesystem;
using namespace ts::tombstone;

namespace {

int fail(const std::string& why) {
  std::cerr << "[smoke] trading post: " << why << '\n';
  return 1;
}

void set_env(const char* k, const std::string& v) {
#if defined(_WIN32)
  _putenv_s(k, v.c_str());
#else
  setenv(k, v.c_str(), 1);
#endif
}

// A one-folder loopback HTTP/1.0 server for the remote-index checks.
class TinyHttp {
 public:
  explicit TinyHttp(fs::path root) : root_(std::move(root)) {}
  ~TinyHttp() { stop(); }

  bool start() {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
    socklen_t len = sizeof(a);
    getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    if (listen(listen_, 8) != 0) return false;
    thread_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() {
    if (!running_.exchange(false)) return;
    // Wake accept() with a throwaway connection.
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<unsigned short>(port_));
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    TS_CLOSESOCK(s);
    if (thread_.joinable()) thread_.join();
    TS_CLOSESOCK(listen_);
  }
  int port() const { return port_; }
  int served() const { return served_; }

 private:
  void loop() {
    while (running_) {
      sock_t c = accept(listen_, nullptr, nullptr);
      if (!running_) {
        TS_CLOSESOCK(c);
        break;
      }
      std::string req;
      char buf[2048];
      while (req.find("\r\n\r\n") == std::string::npos) {
        const int n = static_cast<int>(recv(c, buf, sizeof(buf), 0));
        if (n <= 0) break;
        req.append(buf, static_cast<std::size_t>(n));
      }
      std::string path = "/";
      const std::size_t sp = req.find(' ');
      if (sp != std::string::npos) path = req.substr(sp + 1, req.find(' ', sp + 1) - sp - 1);
      std::string body, head;
      const std::string rel = path.substr(1);
      if (path == "/moved.json") {
        head = "HTTP/1.0 302 Found\r\nLocation: /index.json\r\n\r\n";
      } else if (rel.find("..") == std::string::npos && !rel.empty() &&
                 store::read_text_file((root_ / rel).string(), &body, nullptr)) {
        head = "HTTP/1.0 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
      } else {
        head = "HTTP/1.0 404 Not Found\r\n\r\n";
        body.clear();
      }
      const std::string out = head + body;
      std::size_t sent = 0;
      while (sent < out.size()) {
        const int n = static_cast<int>(send(c, out.data() + sent, static_cast<int>(out.size() - sent), 0));
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
      }
      ++served_;
      TS_CLOSESOCK(c);
    }
  }

  fs::path root_;
  sock_t listen_{};
  int port_ = 0;
  std::atomic<bool> running_{true};
  std::atomic<int> served_{0};
  std::thread thread_;
};

std::string file_url(const fs::path& p) {
  std::string s = fs::absolute(p).generic_string();
  return s[0] == '/' ? "file://" + s : "file:///" + s;
}

std::string find_lantern() {
#if defined(TS_SAMPLE_PLUGIN_DIR)
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(TS_SAMPLE_PLUGIN_DIR, ec)) {
    if (e.path().extension() == plugin::DynamicLibrary::native_extension() &&
        e.path().filename().string().find("lantern") != std::string::npos) {
      return e.path().string();
    }
  }
#endif
  return {};
}

int basics() {
  // SHA-256 (FIPS 180-2 vectors).
  if (store::sha256_hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ||
      store::sha256_hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
      store::sha256_hex(std::string(1000, 'a')) !=
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3") {
    return fail("sha256 vectors");
  }
  // Ed25519 (RFC 8032 test 1 public key from its seed), sign + verify + tamper.
  store::KeyPair kp;
  if (!store::ed25519_keypair_from_seed(
          "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", &kp) ||
      kp.public_hex != "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a") {
    return fail("ed25519 key from RFC 8032 seed: " + kp.public_hex);
  }
  const std::string sig = store::ed25519_sign(kp.secret_hex, "");
  if (sig != "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b") {
    return fail("ed25519 RFC 8032 test 1 signature");
  }
  if (!store::ed25519_verify(kp.public_hex, sig, "") || store::ed25519_verify(kp.public_hex, sig, "x")) {
    return fail("ed25519 verify / tamper");
  }
  // JSON.
  store::Json j;
  if (!store::Json::parse(R"({"a":[1,2,{"b":"\u00e9"}],"c":true,"d":null})", &j) ||
      j.get("a").size() != 3 || j.get("a").at(2).string_at("b") != "\xc3\xa9" ||
      !j.get("c").as_bool() || store::Json::parse("{\"a\":1,}", nullptr) ||
      store::Json::parse("[1] x", nullptr)) {
    return fail("json parse");
  }
  store::Json back;
  if (!store::Json::parse(j.dump(), &back) || back.dump() != j.dump()) return fail("json round trip");
  // URLs.
  if (store::resolve_url("http://h:8080/a/index.json", "x.dll") != "http://h:8080/a/x.dll" ||
      store::resolve_url("http://h/a/index.json", "/p/x.dll") != "http://h/p/x.dll" ||
      store::resolve_url("http://h/a/i.json", "https://o/y.dll") != "https://o/y.dll" ||
      store::file_url_path("file:///C:/x%20y/i.json") != "C:/x y/i.json" ||
      store::file_url_path("file:///tmp/i.json") != "/tmp/i.json" ||
      !store::file_url_path("http://h/i.json").empty()) {
    return fail("url helpers");
  }
  return 0;
}

}  // namespace

int run_store_sign(int argc, char** argv) {
  std::string index, seed;
  for (int i = 1; i + 2 < argc + 0 || i + 2 == argc; ++i) {
    if (std::strcmp(argv[i], "--store-sign") == 0 && i + 2 < argc) {
      index = argv[i + 1];
      seed = argv[i + 2];
      break;
    }
  }
  store::KeyPair kp;
  std::string text, err;
  if (index.empty() || !store::ed25519_keypair_from_seed(seed, &kp) ||
      !store::read_text_file(index, &text, &err)) {
    std::cerr << "Usage: ts_admin --store-sign <index.json> <64 hex seed>  " << err << '\n';
    return 2;
  }
  const std::string sig = store::ed25519_sign(kp.secret_hex, text);
  if (!store::write_text_file(index + ".sig", sig + "\n", &err)) {
    std::cerr << err << '\n';
    return 1;
  }
  std::cout << "public key " << kp.public_hex << "\nwrote " << index << ".sig\n";
  return 0;
}

int run_trading_post_smoke() {
  if (basics() != 0) return 1;

  const fs::path root = fs::absolute(fs::path("TombStoneProjects") / "_trading_post_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path cache = root / "cache";
  fs::create_directories(cache);
  const char* old_env = std::getenv("TS_PLUGIN_CACHE");
  const std::string saved_env = old_env ? old_env : "";
  set_env("TS_PLUGIN_CACHE", cache.string());
  struct Restore {
    std::string v;
    ~Restore() { set_env("TS_PLUGIN_CACHE", v); }
  } restore{saved_env};

  // Built-ins only as the "known" wagon (the build-tree lantern stays out so
  // it can be installed from a source).
  plugin::Catalog known;
  plugins::register_builtin_plugins(known, nullptr);
  editor::register_editor_builtin_plugins(known, nullptr);

  store::TradingPost post(cache.string());
  post.refresh(&known);
  for (const char* id : {"ts.posse", "ts.tumbleweed", "ts.frontier", "ts.shipyard", "ts.ai",
                         "ts.story", "ts.quests", "ts.dialogue", "ts.saves", "ts.profiler", "ts.weather"}) {
    const store::StoreEntry* e = post.find(id);
    if (!e || e->source != store::Source::BuiltIn || !e->installed) {
      return fail(std::string("built-in listed: ") + id);
    }
  }
  if (!post.find("ts.frontier")->optional || post.find("ts.posse")->optional ||
      !post.find("ts.ai")->optional || !post.find("ts.quests")->optional || !post.find("ts.dialogue")->optional ||
      !post.find("ts.saves")->optional || !post.find("ts.story")->optional || !post.find("ts.profiler")->optional ||
      !post.find("ts.weather")->optional) {
    return fail("optional = off by default (ships only when a project enables it)");
  }
  for (const char* id : {"ts.steam"}) {
    const store::StoreEntry* e = post.find(id);
    if (!e || e->source != store::Source::ComingSoon || e->installable()) {
      return fail(std::string("coming soon: ") + id);
    }
  }
  std::string err;
  if (post.install("ts.steam", &err) || err.find("coming soon") == std::string::npos) {
    return fail("coming soon cannot be installed");
  }
  if (post.uninstall("ts.posse", &err)) return fail("built-ins cannot be uninstalled");
  if (post.search("", "Coming Soon").size() != 1 || post.search("survival", "").empty() ||
      post.search("", "Gameplay").empty() || !post.search("zzzz-nothing", "").empty() ||
      post.search("steam", "").empty() || post.search("steam", "").front()->id != "ts.steam") {
    return fail("search / categories");
  }

  const std::string lantern = find_lantern();
  if (lantern.empty()) {
    std::cout << "[smoke] trading post: no sample.lantern library built; install checks skipped\n";
    fs::remove_all(root, ec);
    return 0;
  }
  const std::string ext = plugin::DynamicLibrary::native_extension();

  // --- Local folder --------------------------------------------------------
  const fs::path local = root / "local";
  fs::create_directories(local);
  fs::copy_file(lantern, local / ("lantern" + ext));
  post.sources().local_folders = {local.string()};
  if (!post.save_sources(&err)) return fail(err);
  store::TradingPost post2(cache.string());
  if (!post2.load_sources(&err) || post2.sources().local_folders.size() != 1) {
    return fail("sources persist in trading_post.json");
  }
  post.refresh(&known);
  const store::StoreEntry* le = post.find("sample.lantern");
  if (!le || le->source != store::Source::Local || le->installed || !le->installable() ||
      le->deps.empty() || le->deps[0].id != "ts.tumbleweed") {
    return fail("local folder entry with its deps");
  }
  if (!post.install("sample.lantern", &err)) return fail("local install: " + err);
  const fs::path inst = cache / "sample.lantern";
  if (!fs::exists(inst / "installed.json") || !fs::exists(inst / ("lantern" + ext))) {
    return fail("installed into <cache>/<id>");
  }
  const auto dirs = plugins::plugin_search_dirs("");
  if (std::find(dirs.begin(), dirs.end(), inst.string()) == dirs.end()) {
    return fail("plugin search finds the cache folder");
  }
  {
    plugin::Catalog c;
    std::vector<std::string> errs;
    if (c.scan_folder(inst.string(), &errs) != 1 || !c.find("sample.lantern")) {
      return fail("the installed library loads from the cache");
    }
  }
  post.refresh(&known);
  if (!post.find("sample.lantern")->installed ||
      post.find("sample.lantern")->source != store::Source::Installed ||
      post.search("", "Installed").empty()) {
    return fail("installed after refresh");
  }

  // Enable per project (Project Manager path): deps come along.
  {
    editor::ProjectInfo p;
    p.path = (root / "claim").string();
    p.name = "Claim";
    p.kind = editor::ProjectKind::TwoD;
    p.created = "x";
    if (!editor::ProjectStore::write_project_json(p, &err)) return fail(err);
    std::vector<std::string> also;
    if (!editor::set_plugin_enabled_in_project(p, "sample.lantern", true, post, &err, &also)) {
      return fail("enable: " + err);
    }
    editor::ProjectInfo disk;
    editor::ProjectStore::load_project_json(p.path, &disk, &err);
    auto has = [&](const std::string& id) {
      return std::find(disk.plugins.begin(), disk.plugins.end(), id) != disk.plugins.end();
    };
    if (!has("sample.lantern") || !has("ts.tumbleweed") || !has("ts.posse") ||
        also != std::vector<std::string>{"ts.tumbleweed"}) {
      return fail("enable adds required deps (and keeps defaults)");
    }
    also.clear();
    if (!editor::set_plugin_enabled_in_project(p, "ts.tumbleweed", false, post, &err, &also)) return fail(err);
    editor::ProjectStore::load_project_json(p.path, &disk, &err);
    if (has("sample.lantern") || has("ts.tumbleweed") || !has("ts.posse")) {
      return fail("disable drops what needs it");
    }
    if (editor::set_plugin_enabled_in_project(p, "ts.steam", true, post, &err)) {
      return fail("coming soon cannot be enabled");
    }
    // Story features bring their core along.
    also.clear();
    if (!editor::set_plugin_enabled_in_project(p, "ts.dialogue", true, post, &err, &also)) return fail(err);
    editor::ProjectStore::load_project_json(p.path, &disk, &err);
    if (!has("ts.dialogue") || !has("ts.story") || also != std::vector<std::string>{"ts.story"}) {
      return fail("enabling ts.dialogue should add ts.story");
    }
  }

  if (!post.uninstall("sample.lantern", &err)) return fail("uninstall: " + err);
  {
    const auto after = plugins::plugin_search_dirs("");
    if (std::find(after.begin(), after.end(), inst.string()) != after.end()) {
      return fail("uninstalled plugins leave the search");
    }
  }
  post.refresh(&known);
  if (fs::exists(inst) || post.find("sample.lantern")->installed) return fail("uninstall cleans the cache");

  // --- Remote index (signed) -----------------------------------------------
  const fs::path remote = root / "remote";
  fs::create_directories(remote / "pkgs");
  fs::copy_file(lantern, remote / "pkgs" / ("lantern-0.3.0" + ext));
  std::string lib_bytes;
  store::read_text_file(lantern, &lib_bytes);
  const std::string lib_sha = store::sha256_hex(lib_bytes);
  const std::string key = store::TradingPost::platform_key();
  auto write_index = [&](const std::string& version, const std::string& sha, std::uint64_t size) {
    store::Json idx = store::Json::object();
    idx.set("name", "Smoke Exchange");
    store::Json list = store::Json::array();
    auto pkg = [&](const std::string& id, const std::string& file, const std::string& h,
                   std::uint64_t sz, const std::string& ver) {
      store::Json p = store::Json::object();
      p.set("id", id);
      p.set("name", id == "sample.lantern" ? "Lanterns (remote)" : id);
      p.set("version", ver);
      p.set("category", "Sample");
      p.set("description", "From the smoke index");
      store::Json types = store::Json::array();
      types.push("2d");
      p.set("types", types);
      store::Json deps = store::Json::array();
      store::Json d = store::Json::object();
      d.set("id", "ts.tumbleweed");
      d.set("min", "1.0.0");
      deps.push(d);
      p.set("deps", deps);
      store::Json dl = store::Json::object();
      store::Json one = store::Json::object();
      one.set("url", "pkgs/" + file);
      one.set("sha256", h);
      one.set("size", static_cast<long long>(sz));
      dl.set(key, one);
      p.set("downloads", dl);
      list.push(p);
    };
    pkg("sample.lantern", "lantern-0.3.0" + ext, sha, size, version);
    pkg("acme.mislabel", "lantern-0.3.0" + ext, lib_sha, lib_bytes.size(), "1.0.0");
    pkg("acme.nofile", "missing" + ext, lib_sha, lib_bytes.size(), "1.0.0");
    idx.set("plugins", list);
    store::write_text_file((remote / "index.json").string(), idx.dump());
  };
  store::KeyPair pub, other;
  store::ed25519_keypair_from_seed(std::string(64, '7'), &pub);
  store::ed25519_keypair_from_seed(std::string(64, '3'), &other);
  auto sign_with = [&](const store::KeyPair& k) {
    std::string text;
    store::read_text_file((remote / "index.json").string(), &text);
    store::write_text_file((remote / "index.json.sig").string(), store::ed25519_sign(k.secret_hex, text) + "\n");
  };
  write_index("0.3.0", lib_sha, lib_bytes.size());
  sign_with(pub);

  const std::string url = file_url(remote / "index.json");
  post.sources().local_folders.clear();
  post.sources().index_urls = {url};
  post.sources().trusted_keys = {pub.public_hex};
  post.sources().allow_unsigned = false;
  post.refresh(&known);
  const store::StoreEntry* re = post.find("sample.lantern");
  if (!re || re->source != store::Source::Remote || re->package_sha256 != lib_sha ||
      re->package_url.find("pkgs/lantern-0.3.0") == std::string::npos || !post.find("acme.mislabel")) {
    std::string notes;
    for (const auto& n : post.notes()) notes += n + "; ";
    return fail("signed file:// index listed: " + notes);
  }
  if (post.install("acme.mislabel", &err) || err.find("not \"acme.mislabel\"") == std::string::npos ||
      fs::exists(cache / "acme.mislabel")) {
    return fail("a library that is not the advertised id is refused: " + err);
  }
  if (post.install("acme.nofile", &err) || fs::exists(cache / "acme.nofile")) {
    return fail("a missing download fails cleanly");
  }
  if (!post.install("sample.lantern", &err)) return fail("remote install (file://): " + err);
  if (!fs::exists(cache / "sample.lantern" / ("lantern-0.3.0" + ext))) return fail("remote package in cache");

  // Updates: the index offers a newer version.
  write_index("0.4.0", lib_sha, lib_bytes.size());
  sign_with(pub);
  post.refresh(&known);
  if (!post.find("sample.lantern")->update_available || post.search("", "Updates").size() != 1) {
    return fail("a newer index version shows as an update");
  }
  post.uninstall("sample.lantern", &err);

  // Refusals: hash, size, signature.
  write_index("0.3.0", std::string(64, '0'), lib_bytes.size());
  sign_with(pub);
  post.refresh(&known);
  if (post.install("sample.lantern", &err) || err.find("SHA-256 mismatch") == std::string::npos ||
      fs::exists(cache / "sample.lantern")) {
    return fail("hash mismatch refused: " + err);
  }
  write_index("0.3.0", lib_sha, lib_bytes.size() + 7);
  sign_with(pub);
  post.refresh(&known);
  if (post.install("sample.lantern", &err) || err.find("size") == std::string::npos) {
    return fail("size mismatch refused: " + err);
  }
  write_index("0.3.0", lib_sha, lib_bytes.size());
  sign_with(other);  // not a trusted key
  post.refresh(&known);
  bool refused = false;
  for (const auto& n : post.notes()) refused |= n.find("refused") != std::string::npos;
  if (!refused || post.find("acme.mislabel") ||
      (post.find("sample.lantern") &&
       post.find("sample.lantern")->source == store::Source::Remote)) {
    return fail("an index signed by an untrusted key is refused");
  }
  sign_with(pub);
  {
    // Tampered after signing.
    std::ofstream(remote / "index.json", std::ios::app) << " ";
    post.refresh(&known);
    if (post.find("acme.mislabel")) return fail("a tampered index is refused");
  }
  fs::remove(remote / "index.json.sig");
  post.sources().allow_unsigned = true;
  post.refresh(&known);
  if (!post.find("acme.mislabel")) return fail("allow_unsigned uses an unsigned index (dev)");
  post.sources().allow_unsigned = false;
  write_index("0.3.0", lib_sha, lib_bytes.size());
  sign_with(pub);

  // Over loopback HTTP (with a redirect).
  TinyHttp http(remote);
  if (!http.start()) return fail("could not start the loopback HTTP server");
  const std::string base = "http://127.0.0.1:" + std::to_string(http.port());
  post.sources().index_urls = {base + "/index.json"};
  post.refresh(&known);
  const store::StoreEntry* he = post.find("sample.lantern");
  if (!he || he->source != store::Source::Remote ||
      he->package_url != base + "/pkgs/lantern-0.3.0" + ext) {
    std::string notes;
    for (const auto& n : post.notes()) notes += n + "; ";
    http.stop();
    return fail("signed http:// index listed: " + notes);
  }
  if (!post.install("sample.lantern", &err)) {
    http.stop();
    return fail("remote install (http://): " + err);
  }
  std::string body;
  const bool redirected = store::fetch_url(base + "/moved.json", &body, &err) && body.find("Smoke Exchange") != std::string::npos;
  const bool missing = !store::fetch_url(base + "/nope.json", &body, &err) && err.find("404") != std::string::npos;
  const int served = http.served();
  http.stop();
  if (!redirected || !missing) return fail("http redirect / 404 handling: " + err);
  post.refresh(&known);
  if (!post.find("sample.lantern")->installed) return fail("http install listed after refresh");
  post.uninstall("sample.lantern", &err);

  fs::remove_all(root, ec);
  std::cout << "[smoke] trading post OK (sha256 + Ed25519 RFC 8032 vectors, json, urls; "
            << post.search("", "").size() << " goods: built-ins, 1 coming soon; local folder "
               "install / enable with deps / uninstall via the plugin cache; signed index over file:// and "
               "http:// (" << served << " requests, redirect, 404), update offered; hash / size / "
               "untrusted key / tampered index / mislabelled library refused)\n";
  return 0;
}
