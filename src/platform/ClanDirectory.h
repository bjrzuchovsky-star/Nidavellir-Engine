#pragma once

// Clans over Steam, behind a seam the editor and the smokes share.
// NIDAVELLIR_STEAM plus a running Steam client uses lobbies and
// ISteamNetworkingMessages. Otherwise available() is false and the Team
// panel keeps the IP/port join. Tests install a MockClan with
// set_clan_backend.

#include "net/Transport.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace steam {

struct ClanFriend {
  std::string id;
  std::string name;
  bool in_app = false;     // currently in app 3326740
  std::string lobby_id;    // their open clan, if any
  std::string claim_name;
  std::string engine;
};

struct ClanMember {
  std::string id;
  std::string name;
  bool host = false;
};

struct ClanMeta {
  std::string engine;
  std::string claim;
  std::string claim_name;
  std::string dim;
};

// Empty when the versions can share a clan. Otherwise a sentence for the panel.
std::string clan_version_error(const std::string& mine, const std::string& theirs);

class ClanBackend {
 public:
  virtual ~ClanBackend() = default;
  virtual bool available() const = 0;
  virtual std::string my_id() const = 0;
  virtual std::string persona() const = 0;
  // Small RGBA avatar. False when this backend has none.
  virtual bool avatar(const std::string& id, std::vector<std::uint8_t>* rgba, int* w, int* h) = 0;
  // Friends-only unless invite_only. Lobby id may arrive on a later poll().
  virtual bool host_clan(bool invite_only, const ClanMeta& meta, std::string* error) = 0;
  virtual std::string lobby_id() const = 0;
  virtual std::string host_id() const = 0;
  virtual bool i_am_host() const = 0;
  virtual std::string meta_value(const std::string& key) const = 0;
  virtual std::vector<ClanMember> members() const = 0;
  virtual bool invite(const std::string& friend_id, std::string* error) = 0;
  virtual void open_invite_overlay() = 0;
  virtual std::vector<ClanFriend> friends() = 0;
  virtual std::vector<ClanFriend> friend_clans() = 0;
  virtual bool join_clan(const std::string& lobby_id, std::string* error) = 0;
  virtual void leave() = 0;
  virtual bool kick(const std::string& member_id, std::string* error) = 0;
  // Hand the lobby to another member. False and a shutdown when alone.
  virtual bool migrate(std::string* new_host, std::string* error) = 0;
  virtual std::string take_join_request() = 0;
  virtual void note_connect_lobby(const std::string& id) = 0;
  virtual void poll() = 0;
  virtual const std::string& last_error() const = 0;
};

class MockClan : public ClanBackend {
 public:
  MockClan(std::string id, std::string name, std::string engine);
  static void reset_world();
  void befriend(MockClan& other);

  bool available() const override { return true; }
  std::string my_id() const override { return id_; }
  std::string persona() const override { return name_; }
  bool avatar(const std::string& id, std::vector<std::uint8_t>* rgba, int* w, int* h) override;
  bool host_clan(bool invite_only, const ClanMeta& meta, std::string* error) override;
  std::string lobby_id() const override { return lobby_; }
  std::string host_id() const override;
  bool i_am_host() const override;
  std::string meta_value(const std::string& key) const override;
  std::vector<ClanMember> members() const override;
  bool invite(const std::string& friend_id, std::string* error) override;
  void open_invite_overlay() override { overlay_opened_ = true; }
  std::vector<ClanFriend> friends() override;
  std::vector<ClanFriend> friend_clans() override;
  bool join_clan(const std::string& lobby_id, std::string* error) override;
  void leave() override;
  bool kick(const std::string& member_id, std::string* error) override;
  bool migrate(std::string* new_host, std::string* error) override;
  std::string take_join_request() override;
  void note_connect_lobby(const std::string& id) override;
  void poll() override {}
  const std::string& last_error() const override { return last_error_; }
  bool overlay_opened() const { return overlay_opened_; }
  bool was_kicked() const { return kicked_; }

 private:
  struct Lobby {
    std::string id;
    std::string host;
    bool invite_only = false;
    std::map<std::string, std::string> meta;
    std::vector<std::string> members;
    std::vector<std::string> invites;
    std::string kicked;
  };
  struct User {
    std::string name;
    std::string engine;
    std::vector<std::string> friends;
  };
  static std::map<std::string, User>& users();
  static std::map<std::string, Lobby>& lobbies();
  static int& next_lobby();
  Lobby* mine();
  const Lobby* mine() const;

  std::string id_;
  std::string name_;
  std::string engine_;
  std::string lobby_;
  std::string last_error_;
  std::string join_request_;
  bool overlay_opened_ = false;
  bool kicked_ = false;
};

// The process backend: a steam clan when SteamService::ready(), else a
// no-op (the IP path). Smokes replace it with a MockClan.
ClanBackend& clan_backend();
void set_clan_backend(ClanBackend* backend);

// Remember +connect_lobby before Steam has finished booting.
void note_connect_lobby(const std::string& lobby_id);
std::string take_launch_lobby();

// P2P transport for the team session. Null when Steam is off: the caller
// keeps ENet. The host's own editor connects to its own id in-process.
std::unique_ptr<net::Transport> make_clan_transport();

}  // namespace steam
}  // namespace tombstone
}  // namespace ts
