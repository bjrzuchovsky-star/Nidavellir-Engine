#pragma once

// The Vault's storage: content-addressed blobs on disk plus a SQLite index.
//
//   <root>/vault.db            items, versions, blobs, upload sessions, API keys
//   <root>/blobs/ab/abcd...    one file per SHA-256 (verified on the way in)
//   <root>/uploads/<id>.part   resumable uploads in progress
//
// Used directly by the local backend (a folder every project on the PC
// shares) and by the ts_vault server. Thread-safe (one connection behind a
// mutex; SQLite's busy timeout covers two editors on one folder).

#include "vault/VaultTypes.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace ts {
namespace tombstone {
namespace vault {

struct UploadSession {
  std::string id;
  std::string sha256;
  std::uint64_t size = 0;
  std::uint64_t received = 0;
  bool complete = false;  // the blob is already stored
};

struct PublishResult {
  int version = 0;
  bool created = false;  // false: identical to the latest version (metadata refreshed)
};

struct KeyInfo {
  std::string name;
  bool can_write = false;
  std::string created;
};

class VaultStore {
 public:
  VaultStore() = default;
  ~VaultStore();
  VaultStore(const VaultStore&) = delete;
  VaultStore& operator=(const VaultStore&) = delete;

  bool open(const std::string& root, std::string* error);
  void close();
  bool is_open() const { return db_ != nullptr; }
  const std::string& root() const { return root_; }

  // Blobs.
  bool has_blob(const std::string& sha) const;
  std::string blob_path(const std::string& sha) const;
  std::uint64_t blob_size(const std::string& sha) const;
  // Copies a file in after checking its SHA-256 (expected "" = trust nothing, compute).
  bool put_blob_file(const std::string& src, const std::string& expected_sha, std::string* sha_out,
                     std::string* error);
  bool put_blob_bytes(const std::string& bytes, std::string* sha_out, std::string* error);
  std::vector<std::string> missing_blobs(const std::vector<std::string>& shas) const;

  // Resumable uploads. begin: reuses an open session for the same blob
  // (that is what "resume" means across a dropped connection), or reports
  // complete when the blob is already stored.
  bool begin_upload(const std::string& sha, std::uint64_t size, UploadSession* out, std::string* error);
  bool upload_status(const std::string& id, UploadSession* out) const;
  // HTTP-like codes: 200 ok, 404 unknown session, 409 offset != received
  // (out->received tells where to continue), 413 past the declared size.
  int append_upload(const std::string& id, std::uint64_t offset, const std::string& data,
                    UploadSession* out, std::string* error);
  // 200 stored, 404 unknown, 409 incomplete, 422 hash mismatch (session dropped).
  int finish_upload(const std::string& id, std::string* error);

  // Items.
  bool list(const Query& q, std::vector<Item>* out, std::string* error) const;  // versions without files
  bool get(const std::string& id, Item* out, std::string* error) const;
  // `meta` carries the descriptive fields (name, kind, tags...); `v` the files,
  // deps, note, uploader and thumbnail. Every blob must already be stored.
  bool publish(const Item& meta, const Version& v, PublishResult* out, std::string* error);

  // API keys (server). The key itself is returned once and stored hashed.
  std::string add_key(const std::string& name, bool can_write, std::string* error);
  bool check_key(const std::string& key, KeyInfo* out) const;
  std::vector<KeyInfo> list_keys() const;
  bool revoke_key(const std::string& name);

 private:
  bool exec(const char* sql, std::string* error) const;
  bool load_versions(const std::string& id, bool with_files, std::vector<Version>* out) const;
  bool read_item_row(const std::string& id, Item* out) const;

  std::string root_;
  sqlite3* db_ = nullptr;
  mutable std::recursive_mutex mu_;
};

std::string random_hex(std::size_t bytes);

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
