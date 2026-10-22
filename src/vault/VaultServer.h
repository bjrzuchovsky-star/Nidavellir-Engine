#pragma once

// ts_vault's HTTP JSON API over a VaultStore (see docs/vault.md):
//
//   GET  /api/v1/health                      no auth: {"ok", "server", "version"}
//   GET  /api/v1/whoami                      {"name", "can_write"}
//   GET  /api/v1/items?q=&kind=&tag=&dim=    {"items": [...]} (versions without files)
//   GET  /api/v1/items/<id>                  the item with every version's files
//   POST /api/v1/items/<id>/versions         {"item": {...}, "version": {...}} -> {"version", "created"}
//   POST /api/v1/blobs/missing               {"sha256": [...]} -> {"missing": [...]}
//   GET  /api/v1/blobs/<sha>                 bytes (Range: bytes=N- resumes)
//   POST /api/v1/uploads                     {"sha256", "size"} -> {"id", "received", "complete"}
//   GET  /api/v1/uploads/<id>                {"id", "received", "size", "complete"}
//   PUT  /api/v1/uploads/<id>?offset=N       a chunk -> {"received"} (409 + received on a gap)
//   POST /api/v1/uploads/<id>/complete       verify SHA-256 and store -> {"ok"}
//
// Every route but health needs "Authorization: Bearer <key>" (or
// X-Api-Key); writes need a key made without --read-only. Keys are stored
// as SHA-256 hashes and never logged.

#include "vault/Http.h"
#include "vault/VaultStore.h"

#include <functional>
#include <string>

namespace ts {
namespace tombstone {
namespace vault {

class VaultServer {
 public:
  explicit VaultServer(VaultStore& store) : store_(store) {}
  ~VaultServer() { stop(); }
  bool start(const ServerOptions& opt, std::string* error);
  void stop() { http_.stop(); }
  void wait() { http_.wait(); }
  int port() const { return http_.port(); }
  // The request handler alone (for tests and embedding).
  void handle(const HttpRequest& req, HttpResponse* res);

 private:
  VaultStore& store_;
  HttpServer http_;
  std::function<void(const std::string&)> log_;
};

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
