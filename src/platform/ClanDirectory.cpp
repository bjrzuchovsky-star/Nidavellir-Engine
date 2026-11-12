#include "platform/ClanDirectory.h"

#include "platform/SteamService.h"

#include <algorithm>
#include <cstdint>

namespace ts {
namespace tombstone {
namespace steam {
ClanBackend& steam_clan_backend();
}  // namespace steam
}  // namespace tombstone
}  // namespace ts

namespace ts {
namespace tombstone {
namespace steam {

namespace {

std::string g_launch_lobby;
ClanBackend* g_override = nullptr;

class NullClan : public ClanBackend {
 public:
  bool available() const override { return false; }
  std::string my_id() const override { return {}; }
  std::string persona() const override { return {}; }
  bool avatar(const std::string&, std::vector<std::uint8_t>*, int*, int*) override { return false; }
  bool host_clan(bool, const ClanMeta&, std::string* error) override {
    if (error) *error = "Steam is not running. Host by address instead.";
    return false;
  }
  std::string lobby_id() const override { return {}; }
  std::string host_id() const override { return {}; }
  bool i_am_host() const override { return false; }
  std::string meta_value(const std::string&) const override { return {}; }
  std::vector<ClanMember> members() const override { return {}; }
  bool invite(const std::string&, std::string* error) override {
    if (error) *error = "Steam is not running.";
    return false;
  }
  void open_invite_overlay() override {}
  std::vector<ClanFriend> friends() override { return {}; }
  std::vector<ClanFriend> friend_clans() override { return {}; }
  bool join_clan(const std::string&, std::string* error) override {
    if (error) *error = "Steam is not running. Join by address instead.";
    return false;
  }
  void leave() override {}
  bool kick(const std::string&, std::string* error) override {
    if (error) *error = "Steam is not running.";
    return false;
  }
  bool migrate(std::string*, std::string* error) override {
    if (error) *error = "Steam is not running.";
    return false;
  }
  std::string take_join_request() override {
    std::string id;
    id.swap(g_launch_lobby);
    return id;
  }
  void note_connect_lobby(const std::string& id) override { g_launch_lobby = id; }
  void poll() override {}
  const std::string& last_error() const override { return last_error_; }

 private:
  std::string last_error_;
};

NullClan& null_clan() {
  static NullClan n;
  return n;
}

}  // namespace

std::string clan_version_error(const std::string& mine, const std::string& theirs) {
  if (mine.empty() || theirs.empty() || mine == theirs) return {};
  return "This clan was forged with Nidavellir Engine " + theirs + ". This editor is " + mine +
         ". Install the same version, then join the clan again.";
}

ClanBackend& clan_backend() {
  if (g_override) return *g_override;
  if (SteamService::instance().ready()) return steam_clan_backend();
  return null_clan();
}

void set_clan_backend(ClanBackend* backend) { g_override = backend; }

std::string take_launch_lobby() {
  std::string id;
  id.swap(g_launch_lobby);
  return id;
}

void note_connect_lobby(const std::string& lobby_id) {
  g_launch_lobby = lobby_id;
  if (g_override) g_override->note_connect_lobby(lobby_id);
  else if (SteamService::instance().ready()) steam_clan_backend().note_connect_lobby(lobby_id);
}

std::map<std::string, MockClan::User>& MockClan::users() {
  static std::map<std::string, User> u;
  return u;
}
std::map<std::string, MockClan::Lobby>& MockClan::lobbies() {
  static std::map<std::string, Lobby> l;
  return l;
}
int& MockClan::next_lobby() {
  static int n = 1;
  return n;
}

void MockClan::reset_world() {
  users().clear();
  lobbies().clear();
  next_lobby() = 1;
}

MockClan::MockClan(std::string id, std::string name, std::string engine)
    : id_(std::move(id)), name_(std::move(name)), engine_(std::move(engine)) {
  users()[id_] = User{name_, engine_, {}};
}

void MockClan::befriend(MockClan& other) {
  users()[id_].friends.push_back(other.id_);
  users()[other.id_].friends.push_back(id_);
}

bool MockClan::avatar(const std::string& id, std::vector<std::uint8_t>* rgba, int* w, int* h) {
  if (!rgba || !users().count(id)) return false;
  *w = 4;
  *h = 4;
  rgba->assign(static_cast<std::size_t>(*w * *h * 4), 0);
  const unsigned tint = static_cast<unsigned>(id.empty() ? 0 : id[0]);
  for (int i = 0; i < *w * *h; ++i) {
    (*rgba)[static_cast<std::size_t>(i * 4 + 0)] = static_cast<std::uint8_t>(40 + tint);
    (*rgba)[static_cast<std::size_t>(i * 4 + 1)] = 90;
    (*rgba)[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>(160 - (tint % 40));
    (*rgba)[static_cast<std::size_t>(i * 4 + 3)] = 255;
  }
  return true;
}

MockClan::Lobby* MockClan::mine() {
  auto it = lobbies().find(lobby_);
  return it == lobbies().end() ? nullptr : &it->second;
}
const MockClan::Lobby* MockClan::mine() const {
  auto it = lobbies().find(lobby_);
  return it == lobbies().end() ? nullptr : &it->second;
}

bool MockClan::host_clan(bool invite_only, const ClanMeta& meta, std::string* error) {
  last_error_.clear();
  if (!lobby_.empty()) leave();
  Lobby L;
  L.id = std::to_string(next_lobby()++);
  L.host = id_;
  L.invite_only = invite_only;
  L.meta["engine"] = meta.engine.empty() ? engine_ : meta.engine;
  L.meta["claim"] = meta.claim;
  L.meta["claim_name"] = meta.claim_name;
  L.meta["dim"] = meta.dim;
  L.meta["app"] = "3326740";
  L.meta["host"] = id_;
  L.members.push_back(id_);
  engine_ = L.meta["engine"];
  users()[id_].engine = engine_;
  lobbies()[L.id] = L;
  lobby_ = L.id;
  (void)error;
  return true;
}

std::string MockClan::host_id() const {
  const Lobby* L = mine();
  return L ? L->host : std::string();
}
bool MockClan::i_am_host() const { return host_id() == id_ && !lobby_.empty(); }
std::string MockClan::meta_value(const std::string& key) const {
  const Lobby* L = mine();
  if (!L) return {};
  auto it = L->meta.find(key);
  return it == L->meta.end() ? std::string() : it->second;
}

std::vector<ClanMember> MockClan::members() const {
  std::vector<ClanMember> out;
  const Lobby* L = mine();
  if (!L) return out;
  for (const std::string& id : L->members) {
    ClanMember m;
    m.id = id;
    m.host = id == L->host;
    auto it = users().find(id);
    m.name = it == users().end() ? id : it->second.name;
    out.push_back(m);
  }
  return out;
}

bool MockClan::invite(const std::string& friend_id, std::string* error) {
  Lobby* L = mine();
  if (!L || L->host != id_) {
    last_error_ = "Only the host can invite.";
    if (error) *error = last_error_;
    return false;
  }
  if (!users().count(friend_id)) {
    last_error_ = "That Wanderer is not on Steam.";
    if (error) *error = last_error_;
    return false;
  }
  L->invites.push_back(friend_id);
  return true;
}

std::vector<ClanFriend> MockClan::friends() {
  std::vector<ClanFriend> out;
  for (const std::string& fid : users()[id_].friends) {
    if (!users().count(fid)) continue;
    ClanFriend f;
    f.id = fid;
    f.name = users()[fid].name;
    f.in_app = true;
    f.engine = users()[fid].engine;
    for (const auto& [lid, L] : lobbies()) {
      for (const std::string& m : L.members) {
        if (m == fid) {
          f.lobby_id = lid;
          f.claim_name = L.meta.count("claim_name") ? L.meta.at("claim_name") : "";
          f.engine = L.meta.count("engine") ? L.meta.at("engine") : f.engine;
        }
      }
    }
    out.push_back(f);
  }
  return out;
}

std::vector<ClanFriend> MockClan::friend_clans() {
  std::vector<ClanFriend> out;
  for (const ClanFriend& f : friends()) {
    if (!f.lobby_id.empty() && f.lobby_id != lobby_) out.push_back(f);
  }
  return out;
}

bool MockClan::join_clan(const std::string& lobby_id, std::string* error) {
  last_error_.clear();
  kicked_ = false;
  if (!lobby_.empty() && lobby_ != lobby_id) leave();
  auto it = lobbies().find(lobby_id);
  if (it == lobbies().end()) {
    last_error_ = "That clan has already closed.";
    if (error) *error = last_error_;
    return false;
  }
  Lobby& L = it->second;
  if (L.invite_only) {
    bool invited = false;
    for (const std::string& i : L.invites) invited = invited || i == id_;
    if (!invited && L.host != id_) {
      last_error_ = "This clan is invite-only.";
      if (error) *error = last_error_;
      return false;
    }
  }
  const std::string theirs = L.meta.count("engine") ? L.meta.at("engine") : "";
  const std::string mismatch = clan_version_error(engine_, theirs);
  if (!mismatch.empty()) {
    last_error_ = mismatch;
    if (error) *error = last_error_;
    return false;
  }
  if (std::find(L.members.begin(), L.members.end(), id_) == L.members.end()) L.members.push_back(id_);
  lobby_ = lobby_id;
  return true;
}

void MockClan::leave() {
  auto it = lobbies().find(lobby_);
  const std::string left = id_;
  lobby_.clear();
  if (it == lobbies().end()) return;
  Lobby& L = it->second;
  L.members.erase(std::remove(L.members.begin(), L.members.end(), left), L.members.end());
  if (L.host == left) {
    if (L.members.empty()) {
      lobbies().erase(it);
      return;
    }
    L.host = L.members.front();
    L.meta["host"] = L.host;
  }
}

bool MockClan::kick(const std::string& member_id, std::string* error) {
  Lobby* L = mine();
  if (!L || L->host != id_) {
    last_error_ = "Only the host can ask a Wanderer to leave.";
    if (error) *error = last_error_;
    return false;
  }
  if (member_id == id_) {
    last_error_ = "The host leaves the clan instead of kicking themselves.";
    if (error) *error = last_error_;
    return false;
  }
  L->members.erase(std::remove(L->members.begin(), L->members.end(), member_id), L->members.end());
  L->kicked = member_id;
  return true;
}

bool MockClan::migrate(std::string* new_host, std::string* error) {
  Lobby* L = mine();
  if (!L) {
    last_error_ = "No clan to hand on.";
    if (error) *error = last_error_;
    return false;
  }
  if (L->members.size() <= 1) {
    leave();
    if (new_host) new_host->clear();
    return true;
  }
  std::string next;
  for (const std::string& m : L->members) {
    if (m != id_) {
      next = m;
      break;
    }
  }
  L->host = next;
  L->meta["host"] = next;
  if (new_host) *new_host = next;
  return true;
}

std::string MockClan::take_join_request() {
  std::string id;
  id.swap(join_request_);
  if (id.empty() && !g_launch_lobby.empty() && g_override == this) {
    id.swap(g_launch_lobby);
  }
  if (!lobby_.empty()) {
    const Lobby* L = mine();
    if (L && L->kicked == id_) {
      kicked_ = true;
      last_error_ = "The host asked you to leave this clan.";
      leave();
    }
  }
  return id;
}

void MockClan::note_connect_lobby(const std::string& id) { join_request_ = id; }

#if !defined(NIDAVELLIR_STEAM)
ClanBackend& steam_clan_backend() { return null_clan(); }
std::unique_ptr<net::Transport> make_clan_transport() { return nullptr; }
#endif

}  // namespace steam
}  // namespace tombstone
}  // namespace ts
