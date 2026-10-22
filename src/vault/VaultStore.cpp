#include "vault/VaultStore.h"

#include "vault/Sha256.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>

namespace ts {
namespace tombstone {
namespace vault {

namespace fs = std::filesystem;
using json::Value;

namespace {

// RAII statement.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr;
  }
  ~Stmt() {
    if (s_) sqlite3_finalize(s_);
  }
  bool ok() const { return s_ != nullptr; }
  Stmt& text(int i, const std::string& v) {
    if (s_) sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& integer(int i, long long v) {
    if (s_) sqlite3_bind_int64(s_, i, v);
    return *this;
  }
  bool row() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
  bool run() {
    if (!s_) return false;
    const int rc = sqlite3_step(s_);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
  }
  std::string col(int i) const {
    const unsigned char* t = sqlite3_column_text(s_, i);
    return t ? reinterpret_cast<const char*>(t) : std::string();
  }
  long long num(int i) const { return sqlite3_column_int64(s_, i); }

 private:
  sqlite3_stmt* s_ = nullptr;
};

std::string json_text(const Value& v) {
  std::string s = json::write(v);
  return s;
}

Value parse_or(const std::string& text, Value fallback) {
  Value v;
  std::string err;
  if (!text.empty() && json::parse(text, &v, &err)) return v;
  return fallback;
}

bool copy_verified(const fs::path& src, const fs::path& dst, const std::string& expected,
                   std::string* sha_out, std::string* error) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  const fs::path tmp = dst.string() + ".tmp" + random_hex(4);
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + src.string();
    return false;
  }
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + tmp.string();
    return false;
  }
  Sha256 h;
  static thread_local char buf[1 << 16];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize n = in.gcount();
    if (n > 0) {
      h.update(buf, static_cast<std::size_t>(n));
      out.write(buf, n);
    }
  }
  out.close();
  const std::string sha = h.hex();
  if (!out || (!expected.empty() && sha != expected)) {
    fs::remove(tmp, ec);
    if (error) *error = out ? "content hash mismatch for " + src.string() : "write failed: " + tmp.string();
    return false;
  }
  if (sha_out) *sha_out = sha;
  if (fs::exists(dst, ec)) {
    fs::remove(tmp, ec);
    return true;
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (fs::exists(dst)) return true;  // a concurrent writer won the race
    if (error) *error = "cannot store blob: " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

std::string random_hex(std::size_t bytes) {
  static std::mutex mu;
  static std::random_device rd;
  std::lock_guard<std::mutex> lock(mu);
  static const char* kHex = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < bytes; ++i) {
    const unsigned v = rd() & 0xFF;
    out += kHex[v >> 4];
    out += kHex[v & 15];
  }
  return out;
}

VaultStore::~VaultStore() { close(); }

void VaultStore::close() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

bool VaultStore::exec(const char* sql, std::string* error) const {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    if (error) *error = msg ? msg : "sqlite error";
    sqlite3_free(msg);
    return false;
  }
  return true;
}

bool VaultStore::open(const std::string& root, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  close();
  std::error_code ec;
  const fs::path r = fs::u8path(root);
  fs::create_directories(r / "blobs", ec);
  fs::create_directories(r / "uploads", ec);
  if (!fs::is_directory(r / "blobs", ec)) {
    if (error) *error = "cannot create the library folder " + root;
    return false;
  }
  root_ = r.lexically_normal().string();
  const std::string dbp = (r / "vault.db").string();
  if (sqlite3_open_v2(dbp.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    if (error) *error = "cannot open " + dbp + ": " + (db_ ? sqlite3_errmsg(db_) : "sqlite");
    close();
    return false;
  }
  sqlite3_busy_timeout(db_, 8000);
  const char* schema =
      "PRAGMA journal_mode=WAL;"
      "CREATE TABLE IF NOT EXISTS items(id TEXT PRIMARY KEY, name TEXT, kind TEXT, author TEXT,"
      " licence TEXT, description TEXT, tags TEXT, dims TEXT, thumb TEXT, latest INTEGER,"
      " created TEXT, updated TEXT);"
      "CREATE TABLE IF NOT EXISTS versions(item TEXT, version INTEGER, created TEXT, note TEXT,"
      " uploader TEXT, size INTEGER, thumb TEXT, manifest TEXT, PRIMARY KEY(item, version));"
      "CREATE TABLE IF NOT EXISTS blobs(sha TEXT PRIMARY KEY, size INTEGER, created TEXT);"
      "CREATE TABLE IF NOT EXISTS uploads(id TEXT PRIMARY KEY, sha TEXT, size INTEGER, created TEXT);"
      "CREATE TABLE IF NOT EXISTS keys(name TEXT PRIMARY KEY, hash TEXT UNIQUE, can_write INTEGER,"
      " created TEXT);"
      "CREATE TABLE IF NOT EXISTS info(k TEXT PRIMARY KEY, v TEXT);"
      "INSERT OR IGNORE INTO info(k, v) VALUES('schema', '1');";
  if (!exec(schema, error)) {
    close();
    return false;
  }
  return true;
}

std::string VaultStore::blob_path(const std::string& sha) const {
  if (!is_sha256_hex(sha)) return {};
  return (fs::u8path(root_) / "blobs" / sha.substr(0, 2) / sha).string();
}

bool VaultStore::has_blob(const std::string& sha) const {
  const std::string p = blob_path(sha);
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(fs::u8path(p), ec);
}

std::uint64_t VaultStore::blob_size(const std::string& sha) const {
  std::error_code ec;
  const auto n = fs::file_size(fs::u8path(blob_path(sha)), ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

std::vector<std::string> VaultStore::missing_blobs(const std::vector<std::string>& shas) const {
  std::vector<std::string> out;
  for (const std::string& s : shas) {
    if (!has_blob(s) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  }
  return out;
}

bool VaultStore::put_blob_file(const std::string& src, const std::string& expected, std::string* sha_out,
                               std::string* error) {
  std::string sha = expected;
  if (sha.empty()) sha = sha256_file(src);
  if (!is_sha256_hex(sha)) {
    if (error) *error = "cannot read " + src;
    return false;
  }
  if (has_blob(sha)) {
    if (sha_out) *sha_out = sha;
    return true;
  }
  if (!copy_verified(fs::u8path(src), fs::u8path(blob_path(sha)), sha, sha_out, error)) return false;
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Stmt(db_, "INSERT OR IGNORE INTO blobs(sha, size, created) VALUES(?, ?, ?)")
      .text(1, sha).integer(2, static_cast<long long>(blob_size(sha))).text(3, utc_now_iso()).run();
  return true;
}

bool VaultStore::put_blob_bytes(const std::string& bytes, std::string* sha_out, std::string* error) {
  const std::string sha = sha256_hex(bytes);
  if (sha_out) *sha_out = sha;
  if (has_blob(sha)) return true;
  const fs::path dst = fs::u8path(blob_path(sha));
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  const fs::path tmp = dst.string() + ".tmp" + random_hex(4);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      if (error) *error = "cannot write blob";
      return false;
    }
  }
  fs::rename(tmp, dst, ec);
  if (ec && !fs::exists(dst)) {
    if (error) *error = "cannot store blob: " + ec.message();
    return false;
  }
  fs::remove(tmp, ec);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Stmt(db_, "INSERT OR IGNORE INTO blobs(sha, size, created) VALUES(?, ?, ?)")
      .text(1, sha).integer(2, static_cast<long long>(bytes.size())).text(3, utc_now_iso()).run();
  return true;
}

// ---- uploads ---------------------------------------------------------------------

bool VaultStore::begin_upload(const std::string& sha, std::uint64_t size, UploadSession* out,
                              std::string* error) {
  if (!is_sha256_hex(sha)) {
    if (error) *error = "sha256 must be 64 lowercase hex";
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(mu_);
  *out = UploadSession{};
  out->sha256 = sha;
  out->size = size;
  if (has_blob(sha)) {
    out->complete = true;
    out->received = size;
    return true;
  }
  {
    Stmt s(db_, "SELECT id, size FROM uploads WHERE sha = ? ORDER BY created DESC LIMIT 1");
    s.text(1, sha);
    if (s.row() && static_cast<std::uint64_t>(s.num(1)) == size) {
      out->id = s.col(0);
      return upload_status(out->id, out);
    }
  }
  out->id = random_hex(16);
  std::error_code ec;
  fs::create_directories(fs::u8path(root_) / "uploads", ec);
  std::ofstream(fs::u8path(root_) / "uploads" / (out->id + ".part"), std::ios::binary | std::ios::trunc);
  if (!Stmt(db_, "INSERT INTO uploads(id, sha, size, created) VALUES(?, ?, ?, ?)")
           .text(1, out->id).text(2, sha).integer(3, static_cast<long long>(size)).text(4, utc_now_iso()).run()) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  return true;
}

bool VaultStore::upload_status(const std::string& id, UploadSession* out) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Stmt s(db_, "SELECT sha, size FROM uploads WHERE id = ?");
  s.text(1, id);
  if (!s.row()) return false;
  out->id = id;
  out->sha256 = s.col(0);
  out->size = static_cast<std::uint64_t>(s.num(1));
  std::error_code ec;
  const auto n = fs::file_size(fs::u8path(root_) / "uploads" / (id + ".part"), ec);
  out->received = ec ? 0 : static_cast<std::uint64_t>(n);
  out->complete = has_blob(out->sha256);
  return true;
}

int VaultStore::append_upload(const std::string& id, std::uint64_t offset, const std::string& data,
                              UploadSession* out, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!upload_status(id, out)) {
    if (error) *error = "unknown upload";
    return 404;
  }
  if (offset != out->received) {
    if (error) *error = "offset " + std::to_string(offset) + " != received " + std::to_string(out->received);
    return 409;
  }
  if (out->received + data.size() > out->size) {
    if (error) *error = "chunk goes past the declared size";
    return 413;
  }
  std::ofstream f(fs::u8path(root_) / "uploads" / (id + ".part"), std::ios::binary | std::ios::app);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  if (!f) {
    if (error) *error = "cannot write upload";
    return 500;
  }
  out->received += data.size();
  return 200;
}

int VaultStore::finish_upload(const std::string& id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  UploadSession u;
  if (!upload_status(id, &u)) {
    if (error) *error = "unknown upload";
    return 404;
  }
  const fs::path part = fs::u8path(root_) / "uploads" / (id + ".part");
  std::error_code ec;
  if (u.received != u.size) {
    if (error) *error = "upload incomplete (" + std::to_string(u.received) + " of " + std::to_string(u.size) + " bytes)";
    return 409;
  }
  std::string err;
  const bool ok = has_blob(u.sha256) || copy_verified(part, fs::u8path(blob_path(u.sha256)), u.sha256, nullptr, &err);
  fs::remove(part, ec);
  Stmt(db_, "DELETE FROM uploads WHERE id = ?").text(1, id).run();
  if (!ok) {
    if (error) *error = "content does not match its sha256; upload discarded";
    return 422;
  }
  Stmt(db_, "INSERT OR IGNORE INTO blobs(sha, size, created) VALUES(?, ?, ?)")
      .text(1, u.sha256).integer(2, static_cast<long long>(u.size)).text(3, utc_now_iso()).run();
  return 200;
}

// ---- items -----------------------------------------------------------------------

bool VaultStore::read_item_row(const std::string& id, Item* it) const {
  Stmt s(db_, "SELECT id, name, kind, author, licence, description, tags, dims, thumb, latest, created, updated"
              " FROM items WHERE id = ?");
  s.text(1, id);
  if (!s.row()) return false;
  it->id = s.col(0);
  it->name = s.col(1);
  it->kind = s.col(2);
  it->author = s.col(3);
  it->licence = s.col(4);
  it->description = s.col(5);
  it->tags = json_strings(parse_or(s.col(6), Value::array()));
  it->dims = json_strings(parse_or(s.col(7), Value::array()));
  it->thumbnail = s.col(8);
  it->latest = static_cast<int>(s.num(9));
  it->created = s.col(10);
  it->updated = s.col(11);
  return true;
}

bool VaultStore::load_versions(const std::string& id, bool with_files, std::vector<Version>* out) const {
  out->clear();
  Stmt s(db_, "SELECT version, created, note, uploader, size, thumb, manifest FROM versions"
              " WHERE item = ? ORDER BY version");
  s.text(1, id);
  while (s.row()) {
    Version v;
    const Value m = parse_or(s.col(6), Value::object());
    from_json(m, &v);
    v.version = static_cast<int>(s.num(0));
    v.created = s.col(1);
    v.note = s.col(2);
    v.uploader = s.col(3);
    v.size = static_cast<std::uint64_t>(s.num(4));
    v.thumbnail = s.col(5);
    if (!with_files) v.files.clear();
    out->push_back(std::move(v));
  }
  return true;
}

bool VaultStore::list(const Query& q, std::vector<Item>* out, std::string* error) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  out->clear();
  if (!db_) {
    if (error) *error = "library not open";
    return false;
  }
  std::vector<std::string> ids;
  {
    Stmt s(db_, "SELECT id FROM items ORDER BY name COLLATE NOCASE, id");
    while (s.row()) ids.push_back(s.col(0));
  }
  for (const std::string& id : ids) {
    Item it;
    if (!read_item_row(id, &it) || !item_matches(it, q)) continue;
    load_versions(id, false, &it.versions);
    out->push_back(std::move(it));
  }
  return true;
}

bool VaultStore::get(const std::string& id, Item* out, std::string* error) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  *out = Item{};
  if (!db_ || !read_item_row(id, out)) {
    if (error) *error = "no such item: " + id;
    return false;
  }
  return load_versions(id, true, &out->versions);
}

bool VaultStore::publish(const Item& meta, const Version& v_in, PublishResult* out, std::string* error) {
  auto fail = [&](const std::string& e) {
    if (error) *error = e;
    return false;
  };
  if (!valid_item_id(meta.id)) return fail("item id must be a 32-hex GUID");
  static const char* kKinds[] = {"texture", "model", "audio", "font", "prefab"};
  if (std::find_if(std::begin(kKinds), std::end(kKinds), [&](const char* k) { return meta.kind == k; }) ==
      std::end(kKinds)) {
    return fail("unknown kind '" + meta.kind + "'");
  }
  Version v = v_in;
  int mains = 0;
  std::set<std::string> paths;
  v.size = 0;
  for (FileRec& f : v.files) {
    if (!safe_rel_path(f.path)) return fail("unsafe path: " + f.path);
    if (!paths.insert(f.path).second) return fail("duplicate path: " + f.path);
    if (!is_sha256_hex(f.sha256) || !has_blob(f.sha256)) return fail("missing blob for " + f.path);
    f.size = blob_size(f.sha256);
    v.size += f.size;
    if (f.role == "main") ++mains;
    else if (f.role != "meta") f.role = "file";
  }
  if (mains != 1) return fail("a version needs exactly one main file");
  if (!v.thumbnail.empty() && !has_blob(v.thumbnail)) return fail("missing thumbnail blob");
  std::lock_guard<std::recursive_mutex> lock(mu_);
  for (const Dep& d : v.deps) {
    Stmt s(db_, "SELECT 1 FROM versions WHERE item = ? AND version = ?");
    s.text(1, d.item).integer(2, d.version);
    if (!s.row()) return fail("dependency " + d.item + " v" + std::to_string(d.version) + " is not in the library");
  }
  Item cur;
  const bool exists = read_item_row(meta.id, &cur);
  std::vector<Version> versions;
  if (exists) load_versions(meta.id, true, &versions);
  if (exists && !versions.empty() && cur.kind != meta.kind) {
    return fail("item " + meta.id + " is a " + cur.kind + ", not a " + meta.kind);
  }
  // Same content as the latest version: keep its number, refresh metadata.
  auto signature = [](const Version& x) {
    std::vector<std::string> s;
    for (const FileRec& f : x.files) s.push_back(f.path + "=" + f.sha256);
    for (const Dep& d : x.deps) s.push_back("dep:" + d.item + "@" + std::to_string(d.version));
    std::sort(s.begin(), s.end());
    return join(s, "|");
  };
  const std::string now = utc_now_iso();
  if (!versions.empty() && signature(versions.back()) == signature(v)) {
    out->version = versions.back().version;
    out->created = false;
  } else {
    out->version = versions.empty() ? 1 : versions.back().version + 1;
    // Never reuse the number of a deleted version (claims may still hold it).
    {
      Stmt r(db_, "SELECT v FROM info WHERE k = ?");
      r.text(1, "retired:" + meta.id);
      if (r.row()) out->version = std::max(out->version, std::atoi(r.col(0).c_str()) + 1);
    }
    out->created = true;
    v.version = out->version;
    v.created = now;
    Value manifest = to_json(v, true);
    if (!Stmt(db_, "INSERT INTO versions(item, version, created, note, uploader, size, thumb, manifest)"
                   " VALUES(?, ?, ?, ?, ?, ?, ?, ?)")
             .text(1, meta.id).integer(2, v.version).text(3, now).text(4, v.note).text(5, v.uploader)
             .integer(6, static_cast<long long>(v.size)).text(7, v.thumbnail).text(8, json_text(manifest)).run()) {
      return fail(std::string("cannot record the version: ") + sqlite3_errmsg(db_));
    }
  }
  const std::string thumb = !v.thumbnail.empty() ? v.thumbnail : (exists ? cur.thumbnail : std::string());
  const std::string name = meta.name.empty() ? (exists ? cur.name : meta.id) : meta.name;
  if (!Stmt(db_, "INSERT INTO items(id, name, kind, author, licence, description, tags, dims, thumb, latest,"
                 " created, updated) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?11)"
                 " ON CONFLICT(id) DO UPDATE SET name = ?2, author = ?4, licence = ?5, description = ?6,"
                 " tags = ?7, dims = ?8, thumb = ?9, latest = MAX(latest, ?10), updated = ?11")
           .text(1, meta.id).text(2, name).text(3, meta.kind).text(4, meta.author).text(5, meta.licence)
           .text(6, meta.description).text(7, json_text(strings_json(meta.tags)))
           .text(8, json_text(strings_json(normalise_dims(meta.dims)))).text(9, thumb)
           .integer(10, out->version).text(11, now).run()) {
    return fail(std::string("cannot record the item: ") + sqlite3_errmsg(db_));
  }
  return true;
}

// ---- management ------------------------------------------------------------------

std::vector<std::string> VaultStore::dependents(const std::string& id, int version) const {
  std::vector<std::string> out;
  Stmt s(db_, "SELECT v.item, v.version, v.manifest, i.name FROM versions v LEFT JOIN items i ON i.id = v.item"
              " WHERE v.item <> ? ORDER BY i.name, v.version");
  s.text(1, id);
  while (s.row()) {
    Version v;
    from_json(parse_or(s.col(2), Value::object()), &v);
    for (const Dep& d : v.deps) {
      if (d.item == id && (version == 0 || d.version == version)) {
        out.push_back((s.col(3).empty() ? s.col(0) : s.col(3)) + " v" + std::to_string(s.num(1)));
        break;
      }
    }
  }
  return out;
}

bool VaultStore::update_item(const Item& meta, Item* out, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Item cur;
  if (!db_ || !read_item_row(meta.id, &cur)) {
    if (error) *error = "no such item: " + meta.id;
    return false;
  }
  std::string name = meta.name;
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
  if (name.empty()) {
    if (error) *error = "an item needs a name";
    return false;
  }
  if (!Stmt(db_, "UPDATE items SET name = ?2, author = ?3, licence = ?4, description = ?5, tags = ?6, dims = ?7,"
                 " updated = ?8 WHERE id = ?1")
           .text(1, meta.id).text(2, name).text(3, meta.author).text(4, meta.licence).text(5, meta.description)
           .text(6, json_text(strings_json(meta.tags))).text(7, json_text(strings_json(normalise_dims(meta.dims))))
           .text(8, utc_now_iso()).run()) {
    if (error) *error = std::string("cannot update the item: ") + sqlite3_errmsg(db_);
    return false;
  }
  return out ? get(meta.id, out, error) : true;
}

bool VaultStore::remove_item(const std::string& id, bool* conflict, std::string* error) {
  if (conflict) *conflict = false;
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Item cur;
  if (!db_ || !read_item_row(id, &cur)) {
    if (error) *error = "no such item: " + id;
    return false;
  }
  const std::vector<std::string> users = dependents(id, 0);
  if (!users.empty()) {
    if (conflict) *conflict = true;
    if (error) *error = cur.name + " is still needed by " + join(users) + " (delete those first)";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE", error)) return false;
  int retired = cur.latest;
  {
    Stmt r(db_, "SELECT v FROM info WHERE k = ?");
    r.text(1, "retired:" + id);
    if (r.row()) retired = std::max(retired, std::atoi(r.col(0).c_str()));
  }
  const bool ok = Stmt(db_, "DELETE FROM versions WHERE item = ?").text(1, id).run() &&
                  Stmt(db_, "INSERT OR REPLACE INTO info(k, v) VALUES(?, ?)")
                      .text(1, "retired:" + id).text(2, std::to_string(retired)).run() &&
                  Stmt(db_, "DELETE FROM items WHERE id = ?").text(1, id).run();
  if (!ok) {
    exec("ROLLBACK", nullptr);
    if (error) *error = std::string("cannot delete the item: ") + sqlite3_errmsg(db_);
    return false;
  }
  return exec("COMMIT", error);
}

bool VaultStore::remove_version(const std::string& id, int version, bool* conflict, std::string* error) {
  if (conflict) *conflict = false;
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Item cur;
  if (!db_ || !read_item_row(id, &cur)) {
    if (error) *error = "no such item: " + id;
    return false;
  }
  std::vector<Version> versions;
  load_versions(id, false, &versions);
  const auto it = std::find_if(versions.begin(), versions.end(), [&](const Version& v) { return v.version == version; });
  if (it == versions.end()) {
    if (error) *error = cur.name + " has no v" + std::to_string(version);
    return false;
  }
  if (versions.size() == 1) {
    if (error) *error = "v" + std::to_string(version) + " is the only version of " + cur.name + ": delete the item instead";
    return false;
  }
  const std::vector<std::string> users = dependents(id, version);
  if (!users.empty()) {
    if (conflict) *conflict = true;
    if (error) *error = cur.name + " v" + std::to_string(version) + " is still needed by " + join(users);
    return false;
  }
  versions.erase(it);
  const Version& newest = versions.back();
  const std::string thumb = newest.thumbnail.empty() ? cur.thumbnail : newest.thumbnail;
  if (!exec("BEGIN IMMEDIATE", error)) return false;
  int retired = version;
  {
    Stmt r(db_, "SELECT v FROM info WHERE k = ?");
    r.text(1, "retired:" + id);
    if (r.row()) retired = std::max(retired, std::atoi(r.col(0).c_str()));
  }
  const bool ok = Stmt(db_, "DELETE FROM versions WHERE item = ? AND version = ?").text(1, id).integer(2, version).run() &&
                  Stmt(db_, "INSERT OR REPLACE INTO info(k, v) VALUES(?, ?)")
                      .text(1, "retired:" + id).text(2, std::to_string(retired)).run() &&
                  Stmt(db_, "UPDATE items SET latest = ?2, thumb = ?3, updated = ?4 WHERE id = ?1")
                      .text(1, id).integer(2, newest.version).text(3, thumb).text(4, utc_now_iso()).run();
  if (!ok) {
    exec("ROLLBACK", nullptr);
    if (error) *error = std::string("cannot delete the version: ") + sqlite3_errmsg(db_);
    return false;
  }
  return exec("COMMIT", error);
}

// ---- keys ------------------------------------------------------------------------

std::string VaultStore::add_key(const std::string& name, bool can_write, std::string* error) {
  if (name.empty() || name.size() > 64) {
    if (error) *error = "key name must be 1-64 characters";
    return {};
  }
  const std::string key = "tsv_" + random_hex(24);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!Stmt(db_, "INSERT INTO keys(name, hash, can_write, created) VALUES(?, ?, ?, ?)")
           .text(1, name).text(2, sha256_hex(key)).integer(3, can_write ? 1 : 0).text(4, utc_now_iso()).run()) {
    if (error) *error = "a key named '" + name + "' already exists (revoke it first)";
    return {};
  }
  return key;
}

bool VaultStore::check_key(const std::string& key, KeyInfo* out) const {
  if (key.empty()) return false;
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Stmt s(db_, "SELECT name, can_write, created FROM keys WHERE hash = ?");
  s.text(1, sha256_hex(key));
  if (!s.row()) return false;
  if (out) {
    out->name = s.col(0);
    out->can_write = s.num(1) != 0;
    out->created = s.col(2);
  }
  return true;
}

std::vector<KeyInfo> VaultStore::list_keys() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  std::vector<KeyInfo> out;
  Stmt s(db_, "SELECT name, can_write, created FROM keys ORDER BY name");
  while (s.row()) out.push_back({s.col(0), s.num(1) != 0, s.col(2)});
  return out;
}

bool VaultStore::revoke_key(const std::string& name) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Stmt(db_, "DELETE FROM keys WHERE name = ?").text(1, name).run();
  return sqlite3_changes(db_) > 0;
}

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
