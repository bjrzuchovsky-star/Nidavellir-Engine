#include "mmo/MmoStore.h"

#include "sqlite3.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace ts {
namespace tombstone {
namespace mmo {

namespace fs = std::filesystem;

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace {

// RAII reset of a cached statement.
struct Use {
  sqlite3_stmt* s;
  explicit Use(sqlite3_stmt* st) : s(st) {}
  ~Use() {
    if (s) {
      sqlite3_reset(s);
      sqlite3_clear_bindings(s);
    }
  }
};

void bind_text(sqlite3_stmt* s, int i, const std::string& v) {
  sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

std::string col_text(sqlite3_stmt* s, int i) {
  const unsigned char* t = sqlite3_column_text(s, i);
  return t ? std::string(reinterpret_cast<const char*>(t), static_cast<std::size_t>(sqlite3_column_bytes(s, i)))
           : std::string();
}

AccountRow read_account(sqlite3_stmt* s) {
  AccountRow a;
  a.id = sqlite3_column_int64(s, 0);
  a.username = col_text(s, 1);
  a.password = col_text(s, 2);
  a.admin = sqlite3_column_int(s, 3) != 0;
  a.banned_until = sqlite3_column_int64(s, 4);
  a.muted_until = sqlite3_column_int64(s, 5);
  a.created = sqlite3_column_int64(s, 6);
  a.last_login = sqlite3_column_int64(s, 7);
  return a;
}

constexpr const char* kAccountCols =
    "id, username, password, admin, banned_until, muted_until, created, last_login";

}  // namespace

Store::~Store() { close(); }

bool Store::fail(const std::string& what) {
  error_ = what + (db_ ? std::string(": ") + sqlite3_errmsg(db_) : std::string());
  return false;
}

bool Store::exec(const char* sql) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    error_ = msg ? msg : "sqlite error";
    sqlite3_free(msg);
    return false;
  }
  return true;
}

sqlite3_stmt* Store::prepare(const char* sql) {
  auto it = stmts_.find(sql);
  if (it != stmts_.end()) return it->second;
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) {
    fail("prepare");
    return nullptr;
  }
  stmts_[sql] = s;
  return s;
}

bool Store::open(const std::string& path, std::string* error) {
  close();
  path_ = path;
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    fail("open " + path);
    if (error) *error = error_;
    close();
    return false;
  }
  sqlite3_busy_timeout(db_, 2000);
  const bool ok = exec("PRAGMA journal_mode=WAL;") && exec("PRAGMA synchronous=NORMAL;") &&
                  exec("PRAGMA foreign_keys=ON;") &&
                  exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);"
                       "CREATE TABLE IF NOT EXISTS accounts ("
                       " id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE COLLATE NOCASE,"
                       " password TEXT NOT NULL, admin INTEGER NOT NULL DEFAULT 0,"
                       " banned_until INTEGER NOT NULL DEFAULT 0, muted_until INTEGER NOT NULL DEFAULT 0,"
                       " created INTEGER NOT NULL DEFAULT 0, last_login INTEGER NOT NULL DEFAULT 0);"
                       "CREATE TABLE IF NOT EXISTS characters ("
                       " id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL UNIQUE REFERENCES accounts(id),"
                       " name TEXT NOT NULL UNIQUE COLLATE NOCASE, zone TEXT NOT NULL, x REAL NOT NULL,"
                       " y REAL NOT NULL, story TEXT NOT NULL DEFAULT '{}', playtime REAL NOT NULL DEFAULT 0,"
                       " updated INTEGER NOT NULL DEFAULT 0);"
                       "CREATE TABLE IF NOT EXISTS inventory ("
                       " character_id INTEGER NOT NULL REFERENCES characters(id), item TEXT NOT NULL,"
                       " count INTEGER NOT NULL, PRIMARY KEY (character_id, item));"
                       "CREATE TABLE IF NOT EXISTS sessions ("
                       " token TEXT PRIMARY KEY, character_id INTEGER NOT NULL, expires INTEGER NOT NULL);"
                       "INSERT OR IGNORE INTO meta (key, value) VALUES ('schema_version', '1');");
  if (!ok) {
    if (error) *error = "sqlite: " + error_;
    close();
    return false;
  }
  return true;
}

void Store::close() {
  for (auto& [k, s] : stmts_) sqlite3_finalize(s);
  stmts_.clear();
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

int Store::schema_version() {
  sqlite3_stmt* s = prepare("SELECT value FROM meta WHERE key = 'schema_version'");
  if (!s) return 0;
  Use u(s);
  return sqlite3_step(s) == SQLITE_ROW ? std::atoi(col_text(s, 0).c_str()) : 0;
}

std::optional<AccountRow> Store::account(const std::string& username) {
  static const std::string sql = std::string("SELECT ") + kAccountCols + " FROM accounts WHERE username = ?";
  sqlite3_stmt* s = prepare(sql.c_str());
  if (!s) return std::nullopt;
  Use u(s);
  bind_text(s, 1, username);
  if (sqlite3_step(s) != SQLITE_ROW) return std::nullopt;
  return read_account(s);
}

std::optional<AccountRow> Store::account_by_id(std::int64_t id) {
  static const std::string sql = std::string("SELECT ") + kAccountCols + " FROM accounts WHERE id = ?";
  sqlite3_stmt* s = prepare(sql.c_str());
  if (!s) return std::nullopt;
  Use u(s);
  sqlite3_bind_int64(s, 1, id);
  if (sqlite3_step(s) != SQLITE_ROW) return std::nullopt;
  return read_account(s);
}

bool Store::create_account(const std::string& username, const std::string& password_hash, bool admin,
                           std::int64_t* id_out) {
  sqlite3_stmt* s = prepare("INSERT INTO accounts (username, password, admin, created) VALUES (?, ?, ?, ?)");
  if (!s) return false;
  Use u(s);
  bind_text(s, 1, username);
  bind_text(s, 2, password_hash);
  sqlite3_bind_int(s, 3, admin ? 1 : 0);
  sqlite3_bind_int64(s, 4, unix_now());
  if (sqlite3_step(s) != SQLITE_DONE) return fail("create account");
  if (id_out) *id_out = sqlite3_last_insert_rowid(db_);
  return true;
}

namespace {
bool set_int(sqlite3* db, sqlite3_stmt* s, std::int64_t v, std::int64_t id) {
  if (!s) return false;
  Use u(s);
  sqlite3_bind_int64(s, 1, v);
  sqlite3_bind_int64(s, 2, id);
  return sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
}
}  // namespace

bool Store::set_admin(std::int64_t a, bool on) {
  return set_int(db_, prepare("UPDATE accounts SET admin = ? WHERE id = ?"), on ? 1 : 0, a);
}
bool Store::set_banned(std::int64_t a, std::int64_t until) {
  return set_int(db_, prepare("UPDATE accounts SET banned_until = ? WHERE id = ?"), until, a);
}
bool Store::set_muted(std::int64_t a, std::int64_t until) {
  return set_int(db_, prepare("UPDATE accounts SET muted_until = ? WHERE id = ?"), until, a);
}
bool Store::touch_login(std::int64_t a) {
  return set_int(db_, prepare("UPDATE accounts SET last_login = ? WHERE id = ?"), unix_now(), a);
}
bool Store::set_password(std::int64_t a, const std::string& h) {
  sqlite3_stmt* s = prepare("UPDATE accounts SET password = ? WHERE id = ?");
  if (!s) return false;
  Use u(s);
  bind_text(s, 1, h);
  sqlite3_bind_int64(s, 2, a);
  return sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db_) == 1;
}

int Store::account_count() {
  sqlite3_stmt* s = prepare("SELECT COUNT(*) FROM accounts");
  if (!s) return 0;
  Use u(s);
  return sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
}

int Store::character_count() {
  sqlite3_stmt* s = prepare("SELECT COUNT(*) FROM characters");
  if (!s) return 0;
  Use u(s);
  return sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
}

bool Store::load_inventory(CharacterRow* row) {
  sqlite3_stmt* s = prepare("SELECT item, count FROM inventory WHERE character_id = ? ORDER BY item");
  if (!s) return false;
  Use u(s);
  sqlite3_bind_int64(s, 1, row->id);
  row->inventory.clear();
  while (sqlite3_step(s) == SQLITE_ROW) row->inventory[col_text(s, 0)] = sqlite3_column_int(s, 1);
  return true;
}

std::optional<CharacterRow> Store::character_where(const char* sql, std::int64_t id, const std::string* text) {
  sqlite3_stmt* s = prepare(sql);
  if (!s) return std::nullopt;
  CharacterRow r;
  {
    Use u(s);
    if (text) bind_text(s, 1, *text);
    else sqlite3_bind_int64(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW) return std::nullopt;
    r.id = sqlite3_column_int64(s, 0);
    r.account_id = sqlite3_column_int64(s, 1);
    r.name = col_text(s, 2);
    r.zone = col_text(s, 3);
    r.x = static_cast<float>(sqlite3_column_double(s, 4));
    r.y = static_cast<float>(sqlite3_column_double(s, 5));
    r.story = col_text(s, 6);
    r.playtime = sqlite3_column_double(s, 7);
  }
  load_inventory(&r);
  return r;
}

#define TS_CHAR_COLS "SELECT id, account_id, name, zone, x, y, story, playtime FROM characters "

std::optional<CharacterRow> Store::character_of(std::int64_t account) {
  return character_where(TS_CHAR_COLS "WHERE account_id = ?", account, nullptr);
}
std::optional<CharacterRow> Store::character_by_id(std::int64_t id) {
  return character_where(TS_CHAR_COLS "WHERE id = ?", id, nullptr);
}
std::optional<CharacterRow> Store::character_by_name(const std::string& name) {
  return character_where(TS_CHAR_COLS "WHERE name = ?", 0, &name);
}

bool Store::create_character(CharacterRow* row) {
  sqlite3_stmt* s = prepare(
      "INSERT INTO characters (account_id, name, zone, x, y, story, playtime, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  if (!s) return false;
  {
    Use u(s);
    sqlite3_bind_int64(s, 1, row->account_id);
    bind_text(s, 2, row->name);
    bind_text(s, 3, row->zone);
    sqlite3_bind_double(s, 4, row->x);
    sqlite3_bind_double(s, 5, row->y);
    bind_text(s, 6, row->story);
    sqlite3_bind_double(s, 7, row->playtime);
    sqlite3_bind_int64(s, 8, unix_now());
    if (sqlite3_step(s) != SQLITE_DONE) return fail("create character");
  }
  row->id = sqlite3_last_insert_rowid(db_);
  if (!row->inventory.empty()) return save_characters({*row});
  return true;
}

bool Store::save_characters(const std::vector<CharacterRow>& rows) {
  if (rows.empty()) return true;
  if (!exec("BEGIN IMMEDIATE")) return false;
  sqlite3_stmt* up = prepare("UPDATE characters SET zone = ?, x = ?, y = ?, story = ?, playtime = ?, updated = ? WHERE id = ?");
  sqlite3_stmt* del = prepare("DELETE FROM inventory WHERE character_id = ?");
  sqlite3_stmt* ins = prepare("INSERT INTO inventory (character_id, item, count) VALUES (?, ?, ?)");
  bool ok = up && del && ins;
  const std::int64_t now = unix_now();
  for (const CharacterRow& r : rows) {
    if (!ok) break;
    {
      Use u(up);
      bind_text(up, 1, r.zone);
      sqlite3_bind_double(up, 2, r.x);
      sqlite3_bind_double(up, 3, r.y);
      bind_text(up, 4, r.story);
      sqlite3_bind_double(up, 5, r.playtime);
      sqlite3_bind_int64(up, 6, now);
      sqlite3_bind_int64(up, 7, r.id);
      ok = sqlite3_step(up) == SQLITE_DONE;
    }
    if (!ok) break;
    {
      Use u(del);
      sqlite3_bind_int64(del, 1, r.id);
      ok = sqlite3_step(del) == SQLITE_DONE;
    }
    for (const auto& [item, n] : r.inventory) {
      if (!ok || n <= 0) continue;
      Use u(ins);
      sqlite3_bind_int64(ins, 1, r.id);
      bind_text(ins, 2, item);
      sqlite3_bind_int(ins, 3, n);
      ok = sqlite3_step(ins) == SQLITE_DONE;
    }
  }
  if (!ok) {
    fail("save characters");
    exec("ROLLBACK");
    return false;
  }
  return exec("COMMIT");
}

bool Store::put_session(const std::string& digest, std::int64_t character, std::int64_t expires) {
  sqlite3_stmt* s = prepare("INSERT OR REPLACE INTO sessions (token, character_id, expires) VALUES (?, ?, ?)");
  if (!s) return false;
  Use u(s);
  bind_text(s, 1, digest);
  sqlite3_bind_int64(s, 2, character);
  sqlite3_bind_int64(s, 3, expires);
  return sqlite3_step(s) == SQLITE_DONE;
}

std::int64_t Store::session_character(const std::string& digest) {
  sqlite3_stmt* s = prepare("SELECT character_id FROM sessions WHERE token = ? AND expires > ?");
  if (!s) return 0;
  Use u(s);
  bind_text(s, 1, digest);
  sqlite3_bind_int64(s, 2, unix_now());
  return sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
}

bool Store::drop_session(const std::string& digest) {
  sqlite3_stmt* s = prepare("DELETE FROM sessions WHERE token = ?");
  if (!s) return false;
  Use u(s);
  bind_text(s, 1, digest);
  return sqlite3_step(s) == SQLITE_DONE;
}

bool Store::drop_sessions_of(std::int64_t character) {
  sqlite3_stmt* s = prepare("DELETE FROM sessions WHERE character_id = ?");
  if (!s) return false;
  Use u(s);
  sqlite3_bind_int64(s, 1, character);
  return sqlite3_step(s) == SQLITE_DONE;
}

int Store::prune_sessions() {
  sqlite3_stmt* s = prepare("DELETE FROM sessions WHERE expires <= ?");
  if (!s) return 0;
  Use u(s);
  sqlite3_bind_int64(s, 1, unix_now());
  sqlite3_step(s);
  return sqlite3_changes(db_);
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
