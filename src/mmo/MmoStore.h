#pragma once

// Town Hall persistence (ts_mmo): one SQLite database per persistent world,
// <project>/mmo/world.db by default. WAL journal, prepared statements,
// batched saves in one transaction.
//
//   accounts   id, username (unique, case-insensitive), password hash,
//              admin, banned_until, muted_until, created, last_login
//   characters id, account_id (one rider per account), name (unique),
//              zone, x, y, story (JSON: {"vars":..,"quests":..}), playtime
//   inventory  character_id, item, count
//   sessions   token digest, character_id, expires (session resume)
//   meta       key, value (schema_version)
//
// Times are Unix seconds. 0 = never / not set.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace ts {
namespace tombstone {
namespace mmo {

struct AccountRow {
  std::int64_t id = 0;
  std::string username;
  std::string password;  // stored hash (MmoCrypto)
  bool admin = false;
  std::int64_t banned_until = 0;  // -1 = forever
  std::int64_t muted_until = 0;
  std::int64_t created = 0;
  std::int64_t last_login = 0;
};

struct CharacterRow {
  std::int64_t id = 0;
  std::int64_t account_id = 0;
  std::string name;
  std::string zone;
  float x = 0.0f;
  float y = 0.0f;
  std::string story = "{}";  // {"vars": {...}, "quests": {...}}
  double playtime = 0.0;
  std::map<std::string, int> inventory;
};

std::int64_t unix_now();

class Store {
 public:
  static constexpr int kSchemaVersion = 1;
  Store() = default;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Creates the folder, the file and the tables when missing.
  bool open(const std::string& path, std::string* error);
  void close();
  bool is_open() const { return db_ != nullptr; }
  const std::string& path() const { return path_; }
  const std::string& last_error() const { return error_; }

  // Accounts. Usernames compare case-insensitively.
  std::optional<AccountRow> account(const std::string& username);
  std::optional<AccountRow> account_by_id(std::int64_t id);
  bool create_account(const std::string& username, const std::string& password_hash, bool admin,
                      std::int64_t* id_out);
  bool set_admin(std::int64_t account, bool admin);
  bool set_banned(std::int64_t account, std::int64_t until);
  bool set_muted(std::int64_t account, std::int64_t until);
  bool set_password(std::int64_t account, const std::string& password_hash);
  bool touch_login(std::int64_t account);
  int account_count();

  // Characters (inventory included).
  std::optional<CharacterRow> character_of(std::int64_t account);
  std::optional<CharacterRow> character_by_id(std::int64_t id);
  std::optional<CharacterRow> character_by_name(const std::string& name);
  bool create_character(CharacterRow* row);  // fills row->id
  // Many at once, one transaction (position, zone, story, playtime, inventory).
  bool save_characters(const std::vector<CharacterRow>& rows);
  int character_count();

  // Sessions (resume tokens, stored as digests).
  bool put_session(const std::string& digest, std::int64_t character, std::int64_t expires);
  // Character of a live session (0 = none / expired).
  std::int64_t session_character(const std::string& digest);
  bool drop_session(const std::string& digest);
  bool drop_sessions_of(std::int64_t character);
  int prune_sessions();  // expired rows removed

  int schema_version();

 private:
  bool exec(const char* sql);
  sqlite3_stmt* prepare(const char* sql);
  bool fail(const std::string& what);
  bool load_inventory(CharacterRow* row);
  std::optional<CharacterRow> character_where(const char* sql, std::int64_t id, const std::string* text);

  sqlite3* db_ = nullptr;
  std::string path_;
  std::string error_;
  std::map<const char*, sqlite3_stmt*> stmts_;
};

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
