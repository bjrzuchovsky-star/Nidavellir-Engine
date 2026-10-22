#pragma once

// One interface for both kinds of library:
//   LocalBackend  - a folder on this PC every project shares
//                   (default Documents/TombStone/Vault), a VaultStore opened in place;
//   RemoteBackend - a ts_vault server over HTTP(S) with an API key
//                   (resumable chunked uploads, verified downloads).
// The editor panel, ts_admin --vault and the smoke only talk to Backend.

#include "vault/VaultStore.h"
#include "vault/VaultTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace vault {

class Backend {
 public:
  virtual ~Backend() = default;
  // Stable identity recorded in a project's lock ("local" or the server URL).
  virtual std::string id() const = 0;
  virtual std::string label() const = 0;
  virtual bool remote() const = 0;
  // Reachable + authorised. info: a short human line ("ts_vault 0.1.0, 12 items").
  virtual bool ping(std::string* info, std::string* error) = 0;
  virtual bool list(const Query& q, std::vector<Item>* out, std::string* error) = 0;
  virtual bool get(const std::string& id, Item* out, std::string* error) = 0;
  // Blobs not stored yet (upload only those).
  virtual bool missing(const std::vector<std::string>& shas, std::vector<std::string>* out,
                       std::string* error) = 0;
  virtual bool upload(const std::string& path, const std::string& sha, std::uint64_t size,
                      std::string* error) = 0;
  // Writes the blob to `dest` (tmp + rename) after checking its SHA-256.
  virtual bool download(const std::string& sha, const std::string& dest, std::string* error) = 0;
  virtual bool publish(const Item& meta, const Version& v, PublishResult* out, std::string* error) = 0;
};

class LocalBackend : public Backend {
 public:
  explicit LocalBackend(std::string dir) : dir_(std::move(dir)) {}
  std::string id() const override { return "local"; }
  std::string label() const override { return "Local library (" + dir_ + ")"; }
  bool remote() const override { return false; }
  bool ping(std::string* info, std::string* error) override;
  bool list(const Query& q, std::vector<Item>* out, std::string* error) override;
  bool get(const std::string& id, Item* out, std::string* error) override;
  bool missing(const std::vector<std::string>& shas, std::vector<std::string>* out,
               std::string* error) override;
  bool upload(const std::string& path, const std::string& sha, std::uint64_t size,
              std::string* error) override;
  bool download(const std::string& sha, const std::string& dest, std::string* error) override;
  bool publish(const Item& meta, const Version& v, PublishResult* out, std::string* error) override;
  const std::string& dir() const { return dir_; }

 private:
  bool ensure(std::string* error);
  std::string dir_;
  VaultStore store_;
};

struct RemoteOptions {
  std::uint64_t chunk_size = 1u << 20;  // 1 MiB per PUT
  int timeout_ms = 30000;
  // Test hook (smoke): drop the connection after this many chunks of an
  // upload, as if the network went away. 0 = never.
  int abort_after_chunks = 0;
};

struct RemoteStats {
  std::uint64_t bytes_uploaded = 0;  // chunk payload bytes actually sent
  std::uint64_t bytes_downloaded = 0;
  int chunks = 0;
  int resumed = 0;  // uploads that continued from a non-zero offset
};

class RemoteBackend : public Backend {
 public:
  // url: "http(s)://host:port" (no trailing /api). The key is sent as a
  // Bearer token and never written to logs or error messages.
  RemoteBackend(std::string url, std::string api_key, RemoteOptions opt = {});
  std::string id() const override { return url_; }
  std::string label() const override { return "Server " + url_; }
  bool remote() const override { return true; }
  bool ping(std::string* info, std::string* error) override;
  bool list(const Query& q, std::vector<Item>* out, std::string* error) override;
  bool get(const std::string& id, Item* out, std::string* error) override;
  bool missing(const std::vector<std::string>& shas, std::vector<std::string>* out,
               std::string* error) override;
  bool upload(const std::string& path, const std::string& sha, std::uint64_t size,
              std::string* error) override;
  bool download(const std::string& sha, const std::string& dest, std::string* error) override;
  bool publish(const Item& meta, const Version& v, PublishResult* out, std::string* error) override;

  RemoteOptions& options() { return opt_; }
  const RemoteStats& stats() const { return stats_; }
  void reset_stats() { stats_ = RemoteStats{}; }

 private:
  bool call(const std::string& method, const std::string& path, const std::string& body,
            const std::string& content_type, int* status, json::Value* out, std::string* error);
  std::string url_;
  std::string key_;
  RemoteOptions opt_;
  RemoteStats stats_;
};

// Documents/TombStone/Vault (Windows: the Documents known folder; else
// $XDG_DOCUMENTS_DIR or ~/Documents). TS_VAULT_DIR overrides it.
std::string default_local_library();

// From the editor settings: backend "local" | "remote".
std::unique_ptr<Backend> make_backend(const std::string& backend, const std::string& local_dir,
                                      const std::string& url, const std::string& api_key);

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
