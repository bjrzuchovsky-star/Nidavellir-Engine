// .gma reader / writer, clean-room from the public layout (see Gma.h).
#include "render3d/import/Gma.h"

#include "render3d/import/Lzma.h"
#include "scene/JsonTree.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ts {
namespace tombstone {
namespace render3d {
namespace gma {

namespace fs = std::filesystem;

std::uint32_t crc32(const unsigned char* d, std::size_t n, std::uint32_t crc) {
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

namespace {

struct Reader {
  const std::vector<unsigned char>& d;
  std::size_t p = 0;
  bool bad = false;
  bool need(std::size_t n) {
    if (p + n > d.size()) bad = true;
    return !bad;
  }
  std::uint64_t uint(int bytes) {
    if (!need(bytes)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= std::uint64_t(d[p + i]) << (8 * i);
    p += bytes;
    return v;
  }
  std::string str() {
    std::string s;
    while (p < d.size() && d[p]) s += static_cast<char>(d[p++]);
    if (p >= d.size()) {
      bad = true;
      return s;
    }
    ++p;
    return s;
  }
};

}  // namespace

bool read(std::vector<unsigned char> bytes, Archive* out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = "GMA: " + why;
    return false;
  };
  *out = Archive{};
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "GMAD", 4) != 0) {
    if (!lzma::looks_like_alone(bytes.data(), bytes.size())) {
      return fail("not a .gma (no GMAD header) and not LZMA-compressed");
    }
    std::vector<unsigned char> raw;
    std::string err;
    if (!lzma::decode_alone(bytes.data(), bytes.size(), &raw, &err)) return fail("compressed download: " + err);
    if (raw.size() < 4 || std::memcmp(raw.data(), "GMAD", 4) != 0) {
      return fail("LZMA data decompressed but is not a .gma");
    }
    bytes = std::move(raw);
    out->lzma = true;
  }
  if (bytes.size() < 4 + 1 + 16) return fail("file too short");
  out->bytes = std::move(bytes);
  Reader r{out->bytes, 4};
  out->version = static_cast<int>(r.uint(1));
  if (out->version < 1 || out->version > 3) return fail("unsupported version " + std::to_string(out->version));
  out->steamid = r.uint(8);
  out->timestamp = r.uint(8);
  if (out->version > 1) {
    for (int guard = 0; guard < 4096; ++guard) {
      std::string s = r.str();
      if (r.bad || s.empty()) break;
      out->required.push_back(std::move(s));
    }
  }
  out->name = r.str();
  out->description_raw = r.str();
  out->author = r.str();
  out->addon_version = static_cast<std::int32_t>(r.uint(4));
  if (r.bad) return fail("truncated header");
  out->description = out->description_raw;
  {
    json::Value v;
    std::string err;
    if (!out->description_raw.empty() && out->description_raw[0] == '{' &&
        json::parse(out->description_raw, &v, &err) && v.is_object()) {
      out->description = v.str("description");
      out->type = v.str("type");
      for (const json::Value& t : v["tags"].items()) {
        if (t.is_string()) out->tags.push_back(t.as_string());
      }
    }
  }
  std::uint64_t offset = 0;
  for (std::uint32_t guard = 0; guard < 1000000; ++guard) {
    const std::uint32_t idx = static_cast<std::uint32_t>(r.uint(4));
    if (r.bad) return fail("truncated file table");
    if (idx == 0) break;
    Entry e;
    e.index = idx;
    e.name = r.str();
    e.size = r.uint(8);
    e.crc = static_cast<std::uint32_t>(r.uint(4));
    if (r.bad) return fail("truncated file table");
    if (e.size > (std::uint64_t(1) << 40)) return fail("file '" + e.name + "' has an impossible size");
    e.offset = offset;
    offset += e.size;
    out->files.push_back(std::move(e));
  }
  const std::uint64_t base = r.p;
  if (base + offset > out->bytes.size()) return fail("file data is truncated (archive cut short?)");
  for (Entry& e : out->files) e.offset += base;
  const std::uint64_t end = base + offset;
  if (end + 4 <= out->bytes.size()) {
    out->has_trailing_crc = true;
    for (int i = 0; i < 4; ++i) out->trailing_crc |= std::uint32_t(out->bytes[end + i]) << (8 * i);
  }
  return true;
}

bool read_file(const std::string& path, Archive* out, std::string* error) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "GMA: cannot open " + path;
    return false;
  }
  std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return read(std::move(d), out, error);
}

bool entry_crc_ok(const Archive& a, const Entry& e) {
  if (e.crc == 0) return true;
  return crc32(a.data(e), static_cast<std::size_t>(e.size)) == e.crc;
}

bool archive_crc_ok(const Archive& a) {
  if (!a.has_trailing_crc) return true;
  std::uint64_t end = a.bytes.size() - 4;
  if (!a.files.empty()) end = a.files.back().offset + a.files.back().size;
  // A trailing value of 0 means "not computed" (gmad writes 0 by default).
  return a.trailing_crc == 0 || crc32(a.bytes.data(), static_cast<std::size_t>(end)) == a.trailing_crc;
}

std::string category(const std::string& name) {
  std::string n = name;
  std::replace(n.begin(), n.end(), '\\', '/');
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (n.rfind("models/", 0) == 0) return "models";
  if (n.rfind("materials/", 0) == 0) return "materials";
  if (n.rfind("sound/", 0) == 0) return "sound";
  return "other";
}

std::string safe_relative_path(const std::string& name) {
  std::string n = name;
  std::replace(n.begin(), n.end(), '\\', '/');
  if (n.empty() || n[0] == '/' || n.find(':') != std::string::npos) return {};
  std::string out, part;
  std::size_t i = 0;
  while (i <= n.size()) {
    const std::size_t j = std::min(n.find('/', i), n.size());
    part = n.substr(i, j - i);
    i = j + 1;
    if (part.empty() || part == "." || part == "..") return {};
    for (unsigned char c : part) {
      if (c < 32 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return {};
    }
    if (part.back() == '.' || part.back() == ' ') return {};
    out += (out.empty() ? "" : "/") + part;
  }
  return out;
}

std::vector<unsigned char> write(const std::string& name, const std::string& description_json,
                                 const std::string& author, const std::vector<NewFile>& files,
                                 std::uint64_t steamid, std::uint64_t timestamp) {
  std::vector<unsigned char> d;
  auto u = [&](std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) d.push_back(static_cast<unsigned char>(v >> (8 * i)));
  };
  auto s = [&](const std::string& v) {
    d.insert(d.end(), v.begin(), v.end());
    d.push_back(0);
  };
  d.insert(d.end(), {'G', 'M', 'A', 'D'});
  u(3, 1);
  u(steamid, 8);
  u(timestamp, 8);
  s("");  // no required content
  s(name);
  s(description_json);
  s(author);
  u(1, 4);
  std::uint32_t idx = 1;
  for (const NewFile& f : files) {
    u(idx++, 4);
    s(f.name);
    u(f.data.size(), 8);
    u(crc32(f.data.data(), f.data.size()), 4);
  }
  u(0, 4);
  for (const NewFile& f : files) d.insert(d.end(), f.data.begin(), f.data.end());
  u(crc32(d.data(), d.size()), 4);
  return d;
}

}  // namespace gma
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
