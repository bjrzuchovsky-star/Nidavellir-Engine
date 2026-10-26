#pragma once

// Live Team Claims: project files over the team session itself, for a
// "Host from my editor" posse without a Vault (docs/team-claims.md, 4).
//
// The host's in-process TeamServer answers blob requests from a
// content-addressed store beside its saved session
// (<claim>/.tombstone/team/host/blobs/<aa>/<sha256>); riders (the host's own
// editor too) reach it through SessionBackend, a vault::Backend that speaks
// chunked "blob_has" / "blob_put" / "blob_get" messages over the reliable
// channel. Every blob is checked against its SHA-256 at both ends.

#include "team/TeamRpc.h"
#include "vault/VaultBackend.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

inline constexpr std::size_t kTeamBlobChunk = 48 * 1024;  // raw bytes per message

// The host side: replies for TeamServer's ServerOptions::blobs.
class SessionBlobStore {
 public:
  explicit SessionBlobStore(std::string dir) : dir_(std::move(dir)) {}
  json::Value handle(bool can_write, const json::Value& req);
  std::string blob_path(const std::string& sha) const;
  const std::string& dir() const { return dir_; }

 private:
  std::string dir_;
};

// The rider side: a Backend over the session (blobs only).
class SessionBackend : public vault::Backend {
 public:
  explicit SessionBackend(std::shared_ptr<team::TeamRpc> rpc) : rpc_(std::move(rpc)) {}
  std::string id() const override { return "team-session"; }
  std::string label() const override { return "the team session (host's editor)"; }
  bool remote() const override { return true; }
  bool ping(std::string* info, std::string* error) override;
  bool list(const vault::Query&, std::vector<vault::Item>*, std::string* error) override { return no(error); }
  bool get(const std::string&, vault::Item*, std::string* error) override { return no(error); }
  bool missing(const std::vector<std::string>& shas, std::vector<std::string>* out, std::string* error) override;
  bool upload(const std::string& path, const std::string& sha, std::uint64_t size, std::string* error) override;
  bool download(const std::string& sha, const std::string& dest, std::string* error) override;
  bool publish(const vault::Item&, const vault::Version&, vault::PublishResult*, std::string* error) override {
    return no(error);
  }
  bool update_item(const vault::Item&, vault::Item*, std::string* error) override { return no(error); }
  bool remove_item(const std::string&, std::string* error) override { return no(error); }
  bool remove_version(const std::string&, int, std::string* error) override { return no(error); }

 private:
  static bool no(std::string* error) {
    if (error) *error = "The team session carries files only, not the Vault library.";
    return false;
  }
  std::shared_ptr<team::TeamRpc> rpc_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
