#pragma once

// Source engine addon archives (.gma), read clean-room from the publicly
// documented layout (no Valve / gmad code):
//
//   "GMAD"  u8 version  u64 steamid  u64 timestamp
//   version > 1: required-content strings, terminated by ""
//   name  description (JSON: description / type / tags)  author   (C strings)
//   i32 addon version
//   file table: { u32 index (0 ends the table)  name  i64 size  u32 crc32 }
//   file data, concatenated in table order
//   optional trailing u32 CRC32 of everything before it
//
// Workshop downloads are sometimes LZMA "alone" compressed (often .bin);
// read_gma() detects that and decompresses (render3d/import/Lzma.h).
// The archive is only read - extraction is the caller's (editor/assets/
// GmaImport.h) and every file name is checked by safe_relative_path().

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace gma {

struct Entry {
  std::uint32_t index = 0;
  std::string name;        // as stored ("models/foo/bar.mdl", '/' separated)
  std::uint64_t size = 0;
  std::uint32_t crc = 0;
  std::uint64_t offset = 0;  // into Archive::bytes
};

struct Archive {
  int version = 0;
  std::uint64_t steamid = 0, timestamp = 0;
  std::vector<std::string> required;
  std::string name, description_raw, author;
  std::string description, type;   // from the description JSON (when it is JSON)
  std::vector<std::string> tags;
  std::int32_t addon_version = 0;
  std::vector<Entry> files;
  bool lzma = false;               // was LZMA-compressed
  bool has_trailing_crc = false;
  std::uint32_t trailing_crc = 0;
  std::vector<unsigned char> bytes;  // the (decompressed) archive

  const unsigned char* data(const Entry& e) const { return bytes.data() + e.offset; }
};

std::uint32_t crc32(const unsigned char* data, std::size_t size, std::uint32_t crc = 0);

// Parses (and, when needed, LZMA-decompresses) a .gma held in memory.
bool read(std::vector<unsigned char> bytes, Archive* out, std::string* error);
bool read_file(const std::string& path, Archive* out, std::string* error);

// CRC32 of one file's data matches the table (a 0 table CRC = not stored).
bool entry_crc_ok(const Archive& a, const Entry& e);
// Trailing CRC over the archive body (true when there is none).
bool archive_crc_ok(const Archive& a);

// "models" / "materials" / "sound" / "other" by the path's first folder
// (and Source model siblings), for the import picker.
std::string category(const std::string& name);
// '/' path without "..", drive letters, absolute roots or
// empty segments; "" when the name is unsafe to extract.
std::string safe_relative_path(const std::string& name);

// Builds a .gma (version 3) - tests and tools.
struct NewFile {
  std::string name;
  std::vector<unsigned char> data;
};
std::vector<unsigned char> write(const std::string& name, const std::string& description_json,
                                 const std::string& author, const std::vector<NewFile>& files,
                                 std::uint64_t steamid = 0, std::uint64_t timestamp = 0);

}  // namespace gma
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
