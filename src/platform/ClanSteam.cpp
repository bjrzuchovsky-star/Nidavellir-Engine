#include "platform/ClanDirectory.h"

#include "core/Brand.h"

#if defined(NIDAVELLIR_STEAM)

#include <steam/steam_api.h>

#include <cstdlib>

namespace ts {
namespace tombstone {
namespace steam {

namespace {

std::uint64_t parse_id(const std::string& s) {
  if (s.empty()) return 0;
  return static_cast<std::uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

std::string id_str(std::uint64_t id) { return std::to_string(id); }

class SteamClan final : public ClanBackend {
 public:
  SteamClan()
      : join_req_(this, &SteamClan::on_join_requested) {}

  bool available() const override { return SteamService::instance().ready() && SteamMatchmaking(); }
  std::string my_id() const override {
    return SteamUser() ? id_str(SteamUser()->GetSteamID().ConvertToUint64()) : std::string();
  }
  std::string persona() const override {
    const char* n = SteamFriends() ? SteamFriends()->GetPersonaName() : "";
    return n ? n : "";
  }
  bool avatar(const std::string& id, std::vector<std::uint8_t>* rgba, int* w, int* h) override {
    if (!rgba || !SteamFriends() || !SteamUtils()) return false;
    const int img = SteamFriends()->GetSmallFriendAvatar(CSteamID(parse_id(id)));
    if (img <= 0) return false;
    uint32 tw = 0, th = 0;
    if (!SteamUtils()->GetImageSize(img, &tw, &th) || tw == 0 || th == 0) return false;
    rgba->resize(static_cast<std::size_t>(tw * th * 4));
    if (!SteamUtils()->GetImageRGBA(img, rgba->data(), static_cast<int>(rgba->size()))) return false;
    *w = static_cast<int>(tw);
    *h = static_cast<int>(th);
    return true;
  }

  bool host_clan(bool invite_only, const ClanMeta& meta, std::string* error) override {
    last_error_.clear();
    if (!available()) {
      last_error_ = "Steam is not running. Host by address instead.";
      if (error) *error = last_error_;
      return false;
    }
    meta_ = meta;
    if (meta_.engine.empty()) meta_.engine = "0.0.0";
    const ELobbyType kind = invite_only ? k_ELobbyTypePrivate : k_ELobbyTypeFriendsOnly;
    const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(kind, 8);
    created_.Set(call, this, &SteamClan::on_created);
    return true;
  }

  std::string lobby_id() const override { return lobby_ ? id_str(lobby_) : std::string(); }
  std::string host_id() const override {
    if (!lobby_ || !SteamMatchmaking()) return {};
    return id_str(SteamMatchmaking()->GetLobbyOwner(CSteamID(lobby_)).ConvertToUint64());
  }
  bool i_am_host() const override { return !host_id().empty() && host_id() == my_id(); }
  std::string meta_value(const std::string& key) const override {
    if (!lobby_ || !SteamMatchmaking()) return {};
    const char* v = SteamMatchmaking()->GetLobbyData(CSteamID(lobby_), key.c_str());
    return v ? v : "";
  }
  std::vector<ClanMember> members() const override {
    std::vector<ClanMember> out;
    if (!lobby_ || !SteamMatchmaking()) return out;
    const CSteamID lobby(lobby_);
    const int n = SteamMatchmaking()->GetNumLobbyMembers(lobby);
    const std::string host = host_id();
    for (int i = 0; i < n; ++i) {
      const CSteamID id = SteamMatchmaking()->GetLobbyMemberByIndex(lobby, i);
      ClanMember m;
      m.id = id_str(id.ConvertToUint64());
      m.host = m.id == host;
      const char* name = SteamFriends() ? SteamFriends()->GetFriendPersonaName(id) : "";
      m.name = name && *name ? name : m.id;
      out.push_back(m);
    }
    return out;
  }

  bool invite(const std::string& friend_id, std::string* error) override {
    if (!lobby_ || !SteamMatchmaking()) {
      last_error_ = "Open a clan before inviting.";
      if (error) *error = last_error_;
      return false;
    }
    if (!SteamMatchmaking()->InviteUserToLobby(CSteamID(lobby_), CSteamID(parse_id(friend_id)))) {
      last_error_ = "Steam refused the invite. They may not own Nidavellir Engine.";
      if (error) *error = last_error_;
      return false;
    }
    return true;
  }

  void open_invite_overlay() override {
    if (lobby_ && SteamFriends()) SteamFriends()->ActivateGameOverlayInviteDialog(CSteamID(lobby_));
  }

  std::vector<ClanFriend> friends() override { return collect(false); }
  std::vector<ClanFriend> friend_clans() override { return collect(true); }

  bool join_clan(const std::string& lobby_id, std::string* error) override {
    last_error_.clear();
    if (!available()) {
      last_error_ = "Steam is not running. Join by address instead.";
      if (error) *error = last_error_;
      return false;
    }
    const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(parse_id(lobby_id)));
    entered_.Set(call, this, &SteamClan::on_entered);
    return true;
  }

  void leave() override {
    if (!lobby_ || !SteamMatchmaking()) {
      lobby_ = 0;
      return;
    }
    const CSteamID lobby(lobby_);
    if (SteamMatchmaking()->GetLobbyOwner(lobby) == SteamUser()->GetSteamID()) {
      const int n = SteamMatchmaking()->GetNumLobbyMembers(lobby);
      for (int i = 0; i < n; ++i) {
        const CSteamID id = SteamMatchmaking()->GetLobbyMemberByIndex(lobby, i);
        if (id != SteamUser()->GetSteamID()) {
          SteamMatchmaking()->SetLobbyOwner(lobby, id);
          SteamMatchmaking()->SetLobbyData(lobby, "host", id_str(id.ConvertToUint64()).c_str());
          break;
        }
      }
    }
    SteamMatchmaking()->LeaveLobby(lobby);
    lobby_ = 0;
  }

  bool kick(const std::string& member_id, std::string* error) override {
    if (!i_am_host()) {
      last_error_ = "Only the host can ask a Wanderer to leave.";
      if (error) *error = last_error_;
      return false;
    }
    SteamMatchmaking()->SetLobbyData(CSteamID(lobby_), "kick", member_id.c_str());
    if (SteamNetworkingMessages()) {
      SteamNetworkingIdentity ident;
      ident.SetSteamID64(parse_id(member_id));
      SteamNetworkingMessages()->CloseSessionWithUser(ident);
    }
    return true;
  }

  bool migrate(std::string* new_host, std::string* error) override {
    if (!lobby_) {
      last_error_ = "No clan to hand on.";
      if (error) *error = last_error_;
      return false;
    }
    const int n = SteamMatchmaking()->GetNumLobbyMembers(CSteamID(lobby_));
    if (n <= 1) {
      leave();
      if (new_host) new_host->clear();
      return true;
    }
    std::string next;
    for (const ClanMember& m : members()) {
      if (!m.host) {
        next = m.id;
        break;
      }
    }
    if (!next.empty()) {
      SteamMatchmaking()->SetLobbyOwner(CSteamID(lobby_), CSteamID(parse_id(next)));
      SteamMatchmaking()->SetLobbyData(CSteamID(lobby_), "host", next.c_str());
    }
    if (new_host) *new_host = next;
    return true;
  }

  std::string take_join_request() override {
    std::string id;
    id.swap(join_request_);
    if (id.empty()) id = take_launch_lobby();
    if (lobby_ && meta_value("kick") == my_id()) {
      last_error_ = "The host asked you to leave this clan.";
      leave();
    }
    return id;
  }
  void note_connect_lobby(const std::string& id) override { join_request_ = id; }
  void poll() override {
    if (join_request_.empty()) {
      std::string id = take_launch_lobby();
      if (!id.empty()) join_request_ = std::move(id);
    }
  }
  const std::string& last_error() const override { return last_error_; }

 private:
  void on_created(LobbyCreated_t* p, bool io_fail) {
    if (io_fail || !p || p->m_eResult != k_EResultOK) {
      last_error_ = "Steam could not open a clan hall.";
      return;
    }
    lobby_ = p->m_ulSteamIDLobby;
    const CSteamID id(lobby_);
    SteamMatchmaking()->SetLobbyData(id, "engine", meta_.engine.c_str());
    SteamMatchmaking()->SetLobbyData(id, "claim", meta_.claim.c_str());
    SteamMatchmaking()->SetLobbyData(id, "claim_name", meta_.claim_name.c_str());
    SteamMatchmaking()->SetLobbyData(id, "dim", meta_.dim.c_str());
    SteamMatchmaking()->SetLobbyData(id, "app", "3326740");
    SteamMatchmaking()->SetLobbyData(id, "host", my_id().c_str());
    SteamMatchmaking()->SetLobbyData(id, "name", (meta_.claim_name.empty() ? "Clan" : meta_.claim_name).c_str());
  }

  void on_entered(LobbyEnter_t* p, bool io_fail) {
    if (io_fail || !p || p->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
      last_error_ = "Steam did not let you into that clan.";
      return;
    }
    lobby_ = p->m_ulSteamIDLobby;
  }

  void on_join_requested(GameLobbyJoinRequested_t* p) {
    if (p) join_request_ = id_str(p->m_steamIDLobby.ConvertToUint64());
  }

  std::vector<ClanFriend> collect(bool only_clans) {
    std::vector<ClanFriend> out;
    if (!SteamFriends()) return out;
    const int n = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    for (int i = 0; i < n; ++i) {
      const CSteamID id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
      FriendGameInfo_t game{};
      const bool playing = SteamFriends()->GetFriendGamePlayed(id, &game) &&
                           game.m_gameID.AppID() == brand::kSteamAppId;
      if (!playing) continue;
      ClanFriend f;
      f.id = id_str(id.ConvertToUint64());
      const char* name = SteamFriends()->GetFriendPersonaName(id);
      f.name = name ? name : f.id;
      f.in_app = true;
      if (game.m_steamIDLobby.IsValid()) f.lobby_id = id_str(game.m_steamIDLobby.ConvertToUint64());
      if (only_clans && (f.lobby_id.empty() || f.lobby_id == lobby_id())) continue;
      out.push_back(f);
    }
    return out;
  }

  CCallResult<SteamClan, LobbyCreated_t> created_;
  CCallResult<SteamClan, LobbyEnter_t> entered_;
  CCallback<SteamClan, GameLobbyJoinRequested_t> join_req_;
  ClanMeta meta_;
  std::uint64_t lobby_ = 0;
  std::string last_error_;
  std::string join_request_;
};

SteamClan& clan() {
  static SteamClan c;
  return c;
}

}  // namespace

ClanBackend& steam_clan_backend() { return clan(); }

}  // namespace steam
}  // namespace tombstone
}  // namespace ts

#endif
