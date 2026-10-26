#include "editor/team/TeamFiles.h"

#include "team/TeamDoc.h"
#include "vault/Sha256.h"

#include <filesystem>
#include <fstream>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;
using json::Value;

namespace {

constexpr double kTimeout = 30.0;

Value fail(const std::string& why) {
  Value r = Value::object();
  r.set("ok", Value::boolean(false));
  r.set("error", Value::string(why));
  return r;
}

Value ok() {
  Value r = Value::object();
  r.set("ok", Value::boolean(true));
  return r;
}

std::uint64_t file_size_or0(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

}  // namespace

std::string SessionBlobStore::blob_path(const std::string& sha) const {
  return (fs::u8path(dir_) / sha.substr(0, 2) / sha).string();
}

Value SessionBlobStore::handle(bool can_write, const Value& req) {
  const std::string t = req.str("t");
  std::error_code ec;
  if (t == "blob_has") {
    Value miss = Value::array();
    for (const Value& v : req["shas"].items()) {
      const std::string sha = v.as_string();
      if (!vault::is_sha256_hex(sha)) return fail("Not a SHA-256: " + sha);
      if (!fs::exists(fs::u8path(blob_path(sha)), ec)) miss.push(Value::string(sha));
    }
    Value r = ok();
    r.set("missing", std::move(miss));
    return r;
  }
  if (t == "blob_put") {
    if (!can_write) return fail("Your key is read-only: you can see the posse's files but not place your own.");
    const std::string sha = req.str("sha");
    if (!vault::is_sha256_hex(sha)) return fail("Not a SHA-256.");
    const std::uint64_t size = static_cast<std::uint64_t>(req.num("size", 0));
    const std::uint64_t off = static_cast<std::uint64_t>(req.num("off", 0));
    const fs::path dest = fs::u8path(blob_path(sha));
    if (fs::exists(dest, ec)) {
      Value r = ok();
      r.set("done", Value::boolean(true));
      return r;
    }
    const fs::path part = fs::u8path(dir_) / "parts" / (sha + ".part");
    fs::create_directories(part.parent_path(), ec);
    const std::uint64_t have = file_size_or0(part);
    if (off != have) {
      if (off == 0) {
        fs::remove(part, ec);
      } else {
        Value r = fail("Out of step: the host has " + std::to_string(have) + " bytes of it.");
        r.set("have", Value::number(static_cast<double>(have)));
        return r;
      }
    }
    std::string bytes;
    if (!team::base64_decode(req.str("data"), &bytes)) return fail("Bad file chunk.");
    if (off + bytes.size() > size) return fail("The file is bigger than announced.");
    {
      std::ofstream f(part, std::ios::binary | (off == 0 ? std::ios::trunc : std::ios::app));
      f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (!f) return fail("The host could not write the file (disk full?).");
    }
    Value r = ok();
    if (off + bytes.size() == size) {
      if (vault::sha256_file(part.string()) != sha) {
        fs::remove(part, ec);
        return fail("The file arrived damaged (SHA-256 mismatch); it will be sent again.");
      }
      fs::create_directories(dest.parent_path(), ec);
      fs::rename(part, dest, ec);
      if (ec) return fail("The host could not store the file: " + ec.message());
      r.set("done", Value::boolean(true));
    }
    return r;
  }
  if (t == "blob_get") {
    const std::string sha = req.str("sha");
    if (!vault::is_sha256_hex(sha)) return fail("Not a SHA-256.");
    const fs::path src = fs::u8path(blob_path(sha));
    std::ifstream f(src, std::ios::binary);
    if (!f) return fail("The host does not have that file (" + sha.substr(0, 12) + ").");
    const std::uint64_t size = file_size_or0(src);
    const std::uint64_t off = static_cast<std::uint64_t>(req.num("off", 0));
    std::uint64_t len = static_cast<std::uint64_t>(req.num("len", kTeamBlobChunk));
    if (len > kTeamBlobChunk) len = kTeamBlobChunk;
    if (off > size) return fail("Past the end of the file.");
    if (off + len > size) len = size - off;
    std::string bytes(static_cast<std::size_t>(len), '\0');
    f.seekg(static_cast<std::streamoff>(off));
    f.read(bytes.data(), static_cast<std::streamsize>(len));
    if (static_cast<std::uint64_t>(f.gcount()) != len) return fail("The host could not read the file.");
    Value r = ok();
    r.set("size", Value::number(static_cast<double>(size)));
    r.set("data", Value::string(team::base64_encode(bytes)));
    return r;
  }
  return fail("Unknown file request.");
}

bool SessionBackend::ping(std::string* info, std::string* error) {
  if (!rpc_ || !rpc_->is_open()) {
    if (error) *error = "Not riding with a posse.";
    return false;
  }
  if (info) *info = "team session";
  return true;
}

bool SessionBackend::missing(const std::vector<std::string>& shas, std::vector<std::string>* out,
                             std::string* error) {
  Value req = Value::object();
  req.set("t", Value::string("blob_has"));
  Value arr = Value::array();
  for (const std::string& s : shas) arr.push(Value::string(s));
  req.set("shas", std::move(arr));
  Value r;
  if (!rpc_->call(std::move(req), &r, kTimeout, error)) return false;
  if (out) {
    out->clear();
    for (const Value& v : r["missing"].items()) out->push_back(v.as_string());
  }
  return true;
}

bool SessionBackend::upload(const std::string& path, const std::string& sha, std::uint64_t size,
                            std::string* error) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  std::uint64_t off = 0;
  std::string buf(kTeamBlobChunk, '\0');
  do {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const std::size_t n = static_cast<std::size_t>(f.gcount());
    Value req = Value::object();
    req.set("t", Value::string("blob_put"));
    req.set("sha", Value::string(sha));
    req.set("size", Value::number(static_cast<double>(size)));
    req.set("off", Value::number(static_cast<double>(off)));
    req.set("data", Value::string(team::base64_encode(buf.substr(0, n))));
    Value r;
    if (!rpc_->call(std::move(req), &r, kTimeout, error)) return false;
    off += n;
    if (r.flag("done", false)) return true;
    if (n == 0) break;
  } while (off < size);
  if (error) *error = "the host did not take the whole file";
  return false;
}

bool SessionBackend::download(const std::string& sha, const std::string& dest, std::string* error) {
  const fs::path tmp = fs::u8path(dest + ".dl");
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot write " + dest;
      return false;
    }
    std::uint64_t off = 0, size = 0;
    do {
      Value req = Value::object();
      req.set("t", Value::string("blob_get"));
      req.set("sha", Value::string(sha));
      req.set("off", Value::number(static_cast<double>(off)));
      req.set("len", Value::number(static_cast<double>(kTeamBlobChunk)));
      Value r;
      if (!rpc_->call(std::move(req), &r, kTimeout, error)) {
        f.close();
        fs::remove(tmp, ec);
        return false;
      }
      size = static_cast<std::uint64_t>(r.num("size", 0));
      std::string bytes;
      if (!team::base64_decode(r.str("data"), &bytes)) {
        if (error) *error = "bad file chunk from the host";
        f.close();
        fs::remove(tmp, ec);
        return false;
      }
      f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      off += bytes.size();
      if (bytes.empty()) break;
    } while (off < size);
    if (off != size) {
      if (error) *error = "the file from the host was cut short";
      f.close();
      fs::remove(tmp, ec);
      return false;
    }
  }
  if (vault::sha256_file(tmp.string()) != sha) {
    fs::remove(tmp, ec);
    if (error) *error = "the file arrived damaged (SHA-256 mismatch)";
    return false;
  }
  fs::remove(fs::u8path(dest), ec);
  fs::rename(tmp, fs::u8path(dest), ec);
  if (ec) {
    if (error) *error = "cannot move the file into place: " + ec.message();
    return false;
  }
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
