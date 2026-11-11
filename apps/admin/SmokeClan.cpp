#include "SmokeClan.h"

#include "platform/ClanDirectory.h"

#include <iostream>
#include <string>

namespace ts {
namespace tombstone {

namespace {

int fails = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "[smoke-clan] FAIL: " << what << '\n';
    ++fails;
  }
}

}  // namespace

int run_clan_smoke() {
  using steam::ClanMeta;
  using steam::MockClan;
  fails = 0;
  MockClan::reset_world();

  expect(steam::clan_version_error("0.2.3", "0.2.3").empty(), "same version is fine");
  const std::string mismatch = steam::clan_version_error("0.2.1", "0.2.3");
  expect(mismatch.find("0.2.3") != std::string::npos && mismatch.find("0.2.1") != std::string::npos,
         "mismatch names both versions");
  expect(mismatch.find("Nidavellir Engine") != std::string::npos, "mismatch names the engine");

  MockClan host("1", "Brynhild", "0.2.3");
  MockClan guest("2", "Eir", "0.2.3");
  MockClan old("3", "Skadi", "0.2.1");
  host.befriend(guest);
  host.befriend(old);

  ClanMeta meta;
  meta.engine = "0.2.3";
  meta.claim = "hall";
  meta.claim_name = "Hall";
  meta.dim = "3d";
  std::string err;
  expect(host.host_clan(false, meta, &err), "host a friends-only clan");
  expect(host.i_am_host() && host.meta_value("engine") == "0.2.3", "lobby stores the engine");
  expect(host.meta_value("app") == "3326740", "lobby stores the app id");
  expect(host.members().size() == 1, "host is the first member");

  expect(!old.join_clan(host.lobby_id(), &err), "old editor is refused");
  expect(err.find("0.2.3") != std::string::npos && err.find("0.2.1") != std::string::npos,
         "refusal is readable");

  expect(guest.friend_clans().size() == 1, "friend sees the clan");
  expect(guest.join_clan(host.lobby_id(), &err), "matching version joins");
  expect(host.members().size() == 2, "both wanderers are in the hall");

  std::vector<std::uint8_t> px;
  int w = 0, h = 0;
  expect(host.avatar(guest.my_id(), &px, &w, &h) && w == 4 && px.size() == 64, "avatar bytes");

  expect(host.invite(guest.my_id(), &err), "invite a friend");
  host.open_invite_overlay();
  expect(host.overlay_opened(), "overlay invite was requested");

  MockClan stranger("4", "Njord", "0.2.3");
  expect(host.host_clan(true, meta, &err), "host an invite-only clan");
  expect(!stranger.join_clan(host.lobby_id(), &err), "invite-only refuses a stranger");
  expect(err.find("invite-only") != std::string::npos, "invite-only reason");
  expect(host.invite(stranger.my_id(), &err), "invite the stranger");
  expect(stranger.join_clan(host.lobby_id(), &err), "invited stranger joins");

  expect(host.kick(stranger.my_id(), &err), "host kicks");
  stranger.take_join_request();
  expect(stranger.was_kicked(), "kicked wanderer is told");
  expect(stranger.lobby_id().empty(), "kicked wanderer has left");

  expect(host.invite(guest.my_id(), &err), "invite the friend into this hall");
  expect(guest.join_clan(host.lobby_id(), &err), "friend joins the invite-only hall");
  std::string next;
  expect(host.migrate(&next, &err) && next == guest.my_id(), "host hands the clan on");
  expect(guest.i_am_host(), "the other wanderer is host");
  host.leave();
  guest.leave();
  expect(host.lobby_id().empty() && guest.lobby_id().empty(), "the clan closes when the last wanderer leaves");

  host.note_connect_lobby("4242");
  expect(host.take_join_request() == "4242", "+connect_lobby is queued");

  // The address path is a different backend. With no override, Steam off
  // means the clan directory stays unavailable.
  steam::set_clan_backend(nullptr);
  expect(!steam::clan_backend().available(), "no Steam: clan directory stays quiet");
  expect(!steam::clan_backend().host_clan(false, meta, &err), "host refuses without Steam");
  expect(err.find("address") != std::string::npos, "host points at the address fallback");

  if (fails) {
    std::cerr << "[smoke-clan] " << fails << " failure(s)\n";
    return 1;
  }
  std::cout << "[smoke-clan] ok\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
