#include "vfs/Pak.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace vfs {

namespace fs = std::filesystem;

namespace {
constexpr char kMagic[8] = {'T', 'S', 'P', 'A', 'K', 0, 0, 1};
constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 8 + 8;

void put_u16(std::string& s, std::uint16_t v) {
  for (int i = 0; i < 2; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u32(std::string& s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u64(std::string& s, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

struct Cursor {
  const std::string& s;
  std::size_t at = 0;
  bool ok = true;
  std::uint64_t get(int bytes) {
    if (at + bytes > s.size()) {
      ok = false;
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
      v |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
    }
    at += bytes;
    return v;
  }
  std::string str(std::size_t n) {
    if (at + n > s.size()) {
      ok = false;
      return {};
    }
    std::string out = s.substr(at, n);
    at += n;
    return out;
  }
};

std::uint64_t content_hash_of(const std::vector<PakEntry>& entries) {
  std::uint64_t h = fnv1a64(nullptr, 0);
  for (const PakEntry& e : entries) {
    h = fnv1a64(e.path.data(), e.path.size(), h);
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>((e.hash >> (8 * i)) & 0xFF);
    h = fnv1a64(b, 8, h);
  }
  return h;
}
}  // namespace

std::uint64_t fnv1a64(const void* data, std::size_t size, std::uint64_t seed) {
  std::uint64_t h = seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

std::string hex64(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string normalize_rel(const std::string& rel) {
  std::string s = rel;
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  std::vector<std::string> parts;
  std::string cur;
  auto flush = [&]() -> bool {
    if (cur.empty() || cur == ".") {
    } else if (cur == "..") {
      return false;
    } else {
      parts.push_back(cur);
    }
    cur.clear();
    return true;
  };
  for (char c : s) {
    if (c == '/') {
      if (!flush()) return {};
    } else {
      cur += c;
    }
  }
  if (!flush()) return {};
  std::string out;
  for (const std::string& p : parts) {
    if (!out.empty()) out += '/';
    out += p;
  }
  return out;
}

void PakWriter::add(const std::string& rel, std::string bytes) {
  const std::string key = normalize_rel(rel);
  if (!key.empty()) files_[key] = std::move(bytes);
}

bool PakWriter::add_file(const std::string& rel, const std::string& disk_path,
                         std::string* error) {
  std::ifstream in(fs::path(disk_path), std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + disk_path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  add(rel, ss.str());
  return true;
}

std::uint64_t PakWriter::total_bytes() const {
  std::uint64_t n = 0;
  for (const auto& [k, v] : files_) n += v.size();
  return n;
}

bool PakWriter::write(const std::string& path, std::uint64_t* content_hash,
                      std::string* error) const {
  std::vector<PakEntry> entries;
  std::uint64_t at = kHeaderSize;
  for (const auto& [rel, bytes] : files_) {
    PakEntry e;
    e.path = rel;
    e.offset = at;
    e.size = bytes.size();
    e.stored_size = bytes.size();
    e.hash = fnv1a64(bytes.data(), bytes.size());
    at += bytes.size();
    entries.push_back(std::move(e));
  }
  const std::uint64_t ch = content_hash_of(entries);
  std::string header(kMagic, 8);
  put_u32(header, kPakVersion);
  put_u32(header, static_cast<std::uint32_t>(entries.size()));
  put_u64(header, at);
  put_u64(header, ch);
  std::string index;
  for (const PakEntry& e : entries) {
    put_u16(index, static_cast<std::uint16_t>(e.path.size()));
    index += e.path;
    put_u64(index, e.offset);
    put_u64(index, e.size);
    put_u64(index, e.stored_size);
    put_u32(index, e.flags);
    put_u64(index, e.hash);
  }
  std::error_code ec;
  if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  for (const auto& [rel, bytes] : files_) {
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  out.write(index.data(), static_cast<std::streamsize>(index.size()));
  if (!out) {
    if (error) *error = "short write to " + path;
    return false;
  }
  if (content_hash) *content_hash = ch;
  return true;
}

bool PakReader::open(const std::string& path, std::string* error) {
  entries_.clear();
  by_path_.clear();
  path_ = path;
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::string header(kHeaderSize, '\0');
  in.read(header.data(), static_cast<std::streamsize>(kHeaderSize));
  if (!in || header.compare(0, 8, std::string(kMagic, 8)) != 0) {
    if (error) *error = path + " is not a .tspak";
    return false;
  }
  Cursor h{header, 8};
  const auto version = static_cast<std::uint32_t>(h.get(4));
  const auto count = static_cast<std::uint32_t>(h.get(4));
  const std::uint64_t index_offset = h.get(8);
  content_hash_ = h.get(8);
  if (version != kPakVersion) {
    if (error) *error = path + ": unsupported pak version " + std::to_string(version);
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto file_size = static_cast<std::uint64_t>(in.tellg());
  if (index_offset > file_size) {
    if (error) *error = path + ": index past end of file";
    return false;
  }
  std::string index(static_cast<std::size_t>(file_size - index_offset), '\0');
  in.seekg(static_cast<std::streamoff>(index_offset));
  in.read(index.data(), static_cast<std::streamsize>(index.size()));
  Cursor c{index};
  for (std::uint32_t i = 0; i < count && c.ok; ++i) {
    PakEntry e;
    const auto len = static_cast<std::size_t>(c.get(2));
    e.path = c.str(len);
    e.offset = c.get(8);
    e.size = c.get(8);
    e.stored_size = c.get(8);
    e.flags = static_cast<std::uint32_t>(c.get(4));
    e.hash = c.get(8);
    if (e.offset + e.stored_size > index_offset) c.ok = false;
    by_path_[e.path] = entries_.size();
    entries_.push_back(std::move(e));
  }
  if (!c.ok || content_hash_of(entries_) != content_hash_) {
    if (error) *error = path + ": corrupt index";
    entries_.clear();
    by_path_.clear();
    return false;
  }
  return true;
}

const PakEntry* PakReader::find(const std::string& rel) const {
  const auto it = by_path_.find(normalize_rel(rel));
  return it == by_path_.end() ? nullptr : &entries_[it->second];
}

bool PakReader::read(const std::string& rel, std::string* out, std::string* error) const {
  const PakEntry* e = find(rel);
  if (!e) {
    if (error) *error = rel + " not in " + path_;
    return false;
  }
  if (e->flags != 0) {
    if (error) *error = rel + ": compressed entries not supported by this build";
    return false;
  }
  std::ifstream in(fs::path(path_), std::ios::binary);
  in.seekg(static_cast<std::streamoff>(e->offset));
  out->assign(static_cast<std::size_t>(e->size), '\0');
  in.read(out->data(), static_cast<std::streamsize>(e->size));
  if (!in || fnv1a64(out->data(), out->size()) != e->hash) {
    if (error) *error = rel + ": hash mismatch in " + path_;
    return false;
  }
  return true;
}

}  // namespace vfs
}  // namespace tombstone
}  // namespace ts
