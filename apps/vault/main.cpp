// ts_vault: the self-hosted Vault server (see docs/vault.md). A small
// headless HTTP JSON API over a library folder (SHA-256 blobs + SQLite
// index) with API keys, resumable chunked uploads and verified downloads.
//
//   ts_vault --root /srv/ts_vault --host 0.0.0.0 --port 8740
//   ts_vault --root /srv/ts_vault --add-key brenden          (prints the key once)
//   ts_vault --root /srv/ts_vault --add-key ci --read-only
//   ts_vault --root /srv/ts_vault --list-keys | --revoke-key NAME
//   ts_vault --smoke                                         (self-test on loopback)

#include "vault/Http.h"
#include "vault/Sha256.h"
#include "vault/VaultBackend.h"
#include "vault/VaultServer.h"
#include "vault/VaultStore.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifndef TS_ENGINE_VERSION
#define TS_ENGINE_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
namespace tv = ts::tombstone::vault;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::mutex g_log_mu;
void log_line(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  std::cout << tv::utc_now_iso() << " " << s << std::endl;
}

std::string env_or(const char* k, const std::string& fallback) {
  const char* v = std::getenv(k);
  return v && *v ? v : fallback;
}

void usage() {
  std::cout <<
      "ts_vault " TS_ENGINE_VERSION " - TombStone Vault server\n"
      "  --root DIR          library folder (default $TS_VAULT_ROOT or ./vault-data)\n"
      "  --host ADDR         listen address (default 127.0.0.1; 0.0.0.0 = every interface)\n"
      "  --port N            listen port (default 8740)\n"
      "  --add-key NAME      create an API key and print it once (add --read-only for pull-only)\n"
      "  --list-keys         list key names\n"
      "  --revoke-key NAME   delete a key\n"
      "  --smoke             self-test on loopback and exit\n"
      "  --version\n"
      "You are responsible for having the rights to what is uploaded to your server.\n";
}

int smoke() {
  const fs::path root = fs::temp_directory_path() / ("ts_vault_smoke_" + tv::random_hex(4));
  std::error_code ec;
  fs::remove_all(root, ec);
  int rc = 1;
  {
    tv::VaultStore store;
    std::string err;
    if (!store.open(root.string(), &err)) {
      std::cerr << "[ts_vault smoke] open: " << err << '\n';
      return 1;
    }
    const std::string key = store.add_key("smoke", true, &err);
    tv::VaultServer server(store);
    tv::ServerOptions so;
    so.port = 0;
    if (key.empty() || !server.start(so, &err)) {
      std::cerr << "[ts_vault smoke] start: " << err << '\n';
      return 1;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    tv::RemoteBackend good(url, key), bad(url, "tsv_wrong");
    std::string info;
    const std::string blob = "ts_vault smoke blob";
    const fs::path src = root / "src.bin";
    std::ofstream(src, std::ios::binary) << blob;
    const std::string sha = tv::sha256_hex(blob);
    std::string berr;
    if (!good.ping(&info, &err)) std::cerr << "[ts_vault smoke] ping: " << err << '\n';
    else if (bad.ping(nullptr, &berr)) std::cerr << "[ts_vault smoke] bad key accepted\n";
    else if (!good.upload(src.string(), sha, blob.size(), &err)) std::cerr << "[ts_vault smoke] upload: " << err << '\n';
    else if (!good.download(sha, (root / "back.bin").string(), &err)) std::cerr << "[ts_vault smoke] download: " << err << '\n';
    else if (tv::sha256_file((root / "back.bin").string()) != sha) std::cerr << "[ts_vault smoke] round trip\n";
    else {
      std::cout << "[ts_vault smoke] OK (" << info << ")\n";
      rc = 0;
    }
    server.stop();
  }
  fs::remove_all(root, ec);
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  std::string root = env_or("TS_VAULT_ROOT", "./vault-data");
  std::string host = env_or("TS_VAULT_HOST", "127.0.0.1");
  int port = std::atoi(env_or("TS_VAULT_PORT", "8740").c_str());
  std::string add_key, revoke;
  bool read_only = false, list_keys = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--root") root = next();
    else if (a == "--host") host = next();
    else if (a == "--port") port = std::atoi(next().c_str());
    else if (a == "--add-key") add_key = next();
    else if (a == "--read-only") read_only = true;
    else if (a == "--list-keys") list_keys = true;
    else if (a == "--revoke-key") revoke = next();
    else if (a == "--smoke") return smoke();
    else if (a == "--version") {
      std::cout << "ts_vault " TS_ENGINE_VERSION "\n";
      return 0;
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown option " << a << "\n";
      usage();
      return 2;
    }
  }
  tv::VaultStore store;
  std::string err;
  if (!store.open(root, &err)) {
    std::cerr << "ts_vault: " << err << '\n';
    return 1;
  }
  if (!add_key.empty()) {
    const std::string key = store.add_key(add_key, !read_only, &err);
    if (key.empty()) {
      std::cerr << "ts_vault: " << err << '\n';
      return 1;
    }
    // The only time the key is shown: it is stored as a hash.
    std::cout << "API key for '" << add_key << "'" << (read_only ? " (read-only)" : "") << ":\n" << key
              << "\nPaste it into the editor's Vault settings. It is not shown again.\n";
    return 0;
  }
  if (list_keys) {
    for (const tv::KeyInfo& k : store.list_keys()) {
      std::cout << k.name << "  " << (k.can_write ? "read-write" : "read-only") << "  " << k.created << '\n';
    }
    return 0;
  }
  if (!revoke.empty()) {
    if (!store.revoke_key(revoke)) {
      std::cerr << "ts_vault: no key named '" << revoke << "'\n";
      return 1;
    }
    std::cout << "revoked '" << revoke << "'\n";
    return 0;
  }
  if (store.list_keys().empty()) {
    std::cerr << "ts_vault: no API keys yet. Create one first: ts_vault --root \"" << root
              << "\" --add-key <name>\n";
    return 1;
  }
  tv::VaultServer server(store);
  tv::ServerOptions so;
  so.host = host;
  so.port = port;
  so.log = log_line;
  if (!server.start(so, &err)) {
    std::cerr << "ts_vault: " << err << '\n';
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  log_line("ts_vault " TS_ENGINE_VERSION " serving " + fs::absolute(root).string() + " on http://" + host + ":" +
           std::to_string(server.port()) + " (Ctrl+C to stop)");
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  log_line("stopping");
  server.stop();
  return 0;
}
