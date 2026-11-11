#include "platform/ClanDirectory.h"
#include "platform/SteamService.h"

#if defined(NIDAVELLIR_STEAM)

#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>

#include <deque>
#include <map>
#include <utility>

namespace ts {
namespace tombstone {
namespace steam {

namespace {

struct Pipe {
  std::deque<std::pair<bool, std::vector<std::uint8_t>>> c2s;
  std::deque<std::pair<bool, std::vector<std::uint8_t>>> s2c;
  std::deque<net::TransportEvent> to_listener;
};

std::shared_ptr<Pipe> g_self;
bool g_accept = false;
std::uint64_t g_listen_id = 0;

class SteamMsgTransport final : public net::Transport {
 public:
  explicit SteamMsgTransport(bool listener) : listener_(listener) {}
  ~SteamMsgTransport() override { close(); }

  bool listen(std::uint16_t, std::size_t, std::string* error) override {
    if (!SteamNetworkingMessages() || !SteamUser()) {
      if (error) *error = "Steam networking is not up.";
      return false;
    }
    listener_ = true;
    g_accept = true;
    g_listen_id = SteamUser()->GetSteamID().ConvertToUint64();
    g_self = std::make_shared<Pipe>();
    self_ = g_self;
    return true;
  }

  net::PeerId connect(const std::string& host, std::uint16_t, std::string* error) override {
    std::uint64_t id = 0;
    try {
      id = static_cast<std::uint64_t>(std::stoull(host));
    } catch (...) {
      if (error) *error = "Clan host id is not a Steam id.";
      return net::kNoPeer;
    }
    if (g_self && id == g_listen_id) {
      self_ = g_self;
      self_peer_ = 1;
      net::TransportEvent ev;
      ev.type = net::TransportEvent::Type::Connected;
      ev.peer = self_peer_;
      inbox_.push_back(ev);
      self_->to_listener.push_back(ev);
      return self_peer_;
    }
    SteamNetworkingIdentity ident;
    ident.SetSteamID64(id);
    const char hello = 1;
    const EResult r = SteamNetworkingMessages()->SendMessageToUser(
        ident, &hello, 1, k_nSteamNetworkingSend_Reliable, 0);
    if (r != k_EResultOK) {
      if (error) *error = "Steam could not open a relay to that Wanderer.";
      return net::kNoPeer;
    }
    const net::PeerId peer = adopt(id);
    net::TransportEvent ev;
    ev.type = net::TransportEvent::Type::Connected;
    ev.peer = peer;
    inbox_.push_back(std::move(ev));
    return peer;
  }

  bool send(net::PeerId peer, net::Delivery delivery, const std::uint8_t* data, std::size_t size) override {
    const bool reliable = delivery == net::Delivery::Reliable;
    if (peer == self_peer_ && self_) {
      (listener_ ? self_->s2c : self_->c2s).emplace_back(reliable, std::vector<std::uint8_t>(data, data + size));
      return true;
    }
    auto it = ids_.find(peer);
    if (it == ids_.end() || !SteamNetworkingMessages()) return false;
    SteamNetworkingIdentity ident;
    ident.SetSteamID64(it->second);
    const int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    const int channel = reliable ? 0 : 1;
    return SteamNetworkingMessages()->SendMessageToUser(ident, data, static_cast<std::uint32_t>(size), flags,
                                                        channel) == k_EResultOK;
  }

  void disconnect(net::PeerId peer, std::uint32_t reason) override {
    net::TransportEvent ev;
    ev.type = net::TransportEvent::Type::Disconnected;
    ev.peer = peer;
    ev.reason = reason;
    inbox_.push_back(ev);
    auto it = ids_.find(peer);
    if (it != ids_.end() && SteamNetworkingMessages()) {
      SteamNetworkingIdentity ident;
      ident.SetSteamID64(it->second);
      SteamNetworkingMessages()->CloseSessionWithUser(ident);
      by_steam_.erase(it->second);
      ids_.erase(it);
    }
  }

  void drop(net::PeerId peer) override { disconnect(peer, 0); }

  bool poll(net::TransportEvent* out) override {
    pull_self();
    pull_steam(0, net::Delivery::Reliable);
    pull_steam(1, net::Delivery::Unreliable);
    if (inbox_.empty()) return false;
    *out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
  }

  void flush() override {}
  std::uint16_t port() const override { return 0; }
  net::LinkStats stats(net::PeerId) const override { return {}; }
  void close() override {
    if (listener_) {
      g_accept = false;
      g_listen_id = 0;
      g_self.reset();
    }
    ids_.clear();
    by_steam_.clear();
    inbox_.clear();
  }
  const char* name() const override { return "steam-messages"; }

 private:
  net::PeerId adopt(std::uint64_t steam) {
    auto it = by_steam_.find(steam);
    if (it != by_steam_.end()) return it->second;
    const net::PeerId id = next_++;
    by_steam_[steam] = id;
    ids_[id] = steam;
    return id;
  }

  void pull_self() {
    if (!self_) return;
    if (listener_) {
      self_peer_ = 1;
      while (!self_->to_listener.empty()) {
        inbox_.push_back(std::move(self_->to_listener.front()));
        self_->to_listener.pop_front();
      }
    }
    auto& q = listener_ ? self_->c2s : self_->s2c;
    while (!q.empty()) {
      net::TransportEvent ev;
      ev.type = net::TransportEvent::Type::Received;
      ev.peer = 1;
      ev.delivery = q.front().first ? net::Delivery::Reliable : net::Delivery::Unreliable;
      ev.data = std::move(q.front().second);
      q.pop_front();
      inbox_.push_back(std::move(ev));
    }
  }

  void pull_steam(int channel, net::Delivery how) {
    if (!SteamNetworkingMessages()) return;
    SteamNetworkingMessage_t* msgs[32];
    const int n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(channel, msgs, 32);
    for (int i = 0; i < n; ++i) {
      SteamNetworkingMessage_t* m = msgs[i];
      const std::uint64_t steam = m->m_identityPeer.GetSteamID64();
      const bool fresh = by_steam_.count(steam) == 0;
      const net::PeerId peer = adopt(steam);
      if (fresh) {
        net::TransportEvent up;
        up.type = net::TransportEvent::Type::Connected;
        up.peer = peer;
        inbox_.push_back(up);
      }
      net::TransportEvent ev;
      ev.type = net::TransportEvent::Type::Received;
      ev.peer = peer;
      ev.delivery = how;
      const auto* bytes = static_cast<const std::uint8_t*>(m->m_pData);
      ev.data.assign(bytes, bytes + m->m_cbSize);
      inbox_.push_back(std::move(ev));
      m->Release();
    }
  }

  bool listener_ = false;
  net::PeerId next_ = 2;
  net::PeerId self_peer_ = 0;
  std::shared_ptr<Pipe> self_;
  std::map<std::uint64_t, net::PeerId> by_steam_;
  std::map<net::PeerId, std::uint64_t> ids_;
  std::deque<net::TransportEvent> inbox_;
};

class SteamNetHooks {
 public:
  SteamNetHooks() : session_(this, &SteamNetHooks::on_session) {}

 private:
  void on_session(SteamNetworkingMessagesSessionRequest_t* p) {
    if (g_accept && p && SteamNetworkingMessages())
      SteamNetworkingMessages()->AcceptSessionWithUser(p->m_identityRemote);
  }
  CCallback<SteamNetHooks, SteamNetworkingMessagesSessionRequest_t> session_;
};

SteamNetHooks& hooks() {
  static SteamNetHooks h;
  return h;
}

}  // namespace

std::unique_ptr<net::Transport> make_clan_transport() {
  if (!SteamService::instance().ready()) return nullptr;
  (void)hooks();
  return std::make_unique<SteamMsgTransport>(false);
}

}  // namespace steam
}  // namespace tombstone
}  // namespace ts

#endif
