#pragma once

// .tspak: TombStone's packed content archive. One file, simple index:
//
//   header  "TSPAK\0\0\1" (8) | u32 version | u32 count | u64 index_offset
//           | u64 content_hash
//   blobs   file bytes, back to back
//   index   per entry: u16 path_len | path (utf-8, '/' separated, relative)
//           | u64 offset | u64 size | u64 stored_size | u32 flags | u64 hash
//
// All integers little-endian. hash = FNV-1a 64 of the file bytes;
// content_hash = FNV-1a 64 over every (path, hash) in index order, so two
// paks with the same files hash the same. flags bit 0 is reserved for
// compression (not written yet: entries are stored).

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace vfs {

inline constexpr std::uint32_t kPakVersion = 1;

std::uint64_t fnv1a64(const void* data, std::size_t size,
                      std::uint64_t seed = 1469598103934665603ULL);
std::string hex64(std::uint64_t v);

struct PakEntry {
  std::string path;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint64_t stored_size = 0;
  std::uint32_t flags = 0;
  std::uint64_t hash = 0;
};

// Collects files then writes the archive in one go (paths sorted).
class PakWriter {
 public:
  // rel uses '/' separators; a second add of the same path replaces it.
  void add(const std::string& rel, std::string bytes);
  bool add_file(const std::string& rel, const std::string& disk_path,
                std::string* error);
  std::size_t count() const { return files_.size(); }
  std::uint64_t total_bytes() const;
  // Writes path; fills content_hash when given.
  bool write(const std::string& path, std::uint64_t* content_hash,
             std::string* error) const;

 private:
  std::map<std::string, std::string> files_;
};

// Reads the index on open; file bytes on demand (hash-checked).
class PakReader {
 public:
  bool open(const std::string& path, std::string* error);
  const std::string& path() const { return path_; }
  std::uint64_t content_hash() const { return content_hash_; }
  const std::vector<PakEntry>& entries() const { return entries_; }
  const PakEntry* find(const std::string& rel) const;
  bool read(const std::string& rel, std::string* out, std::string* error) const;

 private:
  std::string path_;
  std::uint64_t content_hash_ = 0;
  std::vector<PakEntry> entries_;
  std::map<std::string, std::size_t> by_path_;
};

// '\' -> '/', strips "./" and leading '/'; "" when it climbs out ("..").
std::string normalize_rel(const std::string& rel);

}  // namespace vfs
}  // namespace tombstone
}  // namespace ts
