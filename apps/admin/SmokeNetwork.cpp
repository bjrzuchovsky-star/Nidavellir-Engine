// --smoke: networking. Transport layer: bytes, ENet over 127.0.0.1, and the
// latency / jitter / loss simulator.

#include "SmokeNetwork.h"

#include "editor/ProjectInfo.h"
#include "editor/screens/Editor2DScreen.h"
#include "mp/GameClient.h"
#include "mp/LocalPosse.h"
#include "mp/PosseSmoke.h"

#include "net/ByteStream.h"
#include "net/SimTransport.h"
#include "net/Transport.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ts::tombstone::net;
using Clock = std::chrono::steady_clock;

struct Check {
  std::string failure;
  bool operator()(bool ok, const std::string& what) {
    if (!ok && failure.empty()) failure = what;
    return ok;
  }
};

// Pump both ends until `done` or `seconds` pass.
bool pump_until(const std::function<void()>& pump, const std::function<bool()>& done,
                double seconds) {
  const auto end = Clock::now() + std::chrono::duration<double>(seconds);
  while (Clock::now() < end) {
    pump();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  pump();
  return done();
}

std::vector<std::uint8_t> bytes_of(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

void codec_checks(Check& check) {
  ByteWriter w;
  w.u8(0xAB);
  w.u16(0xBEEF);
  w.u32(0xDEADBEEFu);
  w.u64(0x0123'4567'89AB'CDEFull);
  w.f32(-12.375f);
  const std::uint64_t vars[] = {0, 1, 127, 128, 300, 16384, 0xFFFFFFFFull, ~0ull};
  for (std::uint64_t v : vars) w.varu(v);
  const std::int64_t ints[] = {0, -1, 1, -64, 64, -100000, 100000};
  for (std::int64_t v : ints) w.vari(v);
  w.str("Dodge City");
  ByteReader r(w.data());
  check(r.u8() == 0xAB && r.u16() == 0xBEEF && r.u32() == 0xDEADBEEFu &&
            r.u64() == 0x0123'4567'89AB'CDEFull && r.f32() == -12.375f,
        "fixed-size fields should roundtrip");
  for (std::uint64_t v : vars) check(r.varu() == v, "varu should roundtrip");
  for (std::int64_t v : ints) check(r.vari() == v, "vari should roundtrip");
  check(r.str() == "Dodge City" && r.at_end(), "strings should roundtrip");
  ByteWriter small;
  small.varu(127);
  small.vari(-64);
  small.vari(63);
  check(small.size() == 3, "small numbers should pack into one byte each");
  // Overruns latch a failure instead of reading garbage.
  ByteReader bad(w.data().data(), 3);
  bad.u32();
  check(!bad.ok() && bad.u8() == 0, "an overrun should fail the reader");
  ByteWriter big;
  big.varu(5000);
  ByteReader liar(big.data());
  check(liar.str() == std::string() && !liar.ok(), "a string longer than the packet should fail");
  check(fnv1a64("a", 1) != fnv1a64("b", 1), "fnv should tell bytes apart");

  std::string host;
  std::uint16_t port = 0;
  check(parse_endpoint("127.0.0.1:4000", 1, &host, &port) && host == "127.0.0.1" && port == 4000,
        "host:port should parse");
  check(parse_endpoint("saloon.example", 24642, &host, &port) && host == "saloon.example" &&
            port == 24642,
        "a bare host should take the default port");
  check(parse_endpoint("[::1]:5", 1, &host, &port) && host == "::1" && port == 5,
        "[v6]:port should parse");
  check(!parse_endpoint("x:0", 1, &host, &port) && !parse_endpoint("x:99999", 1, &host, &port) &&
            !parse_endpoint(":80", 1, &host, &port) && !parse_endpoint("x:8a", 1, &host, &port),
        "bad ports should be refused");
}

struct Ends {
  std::unique_ptr<Transport> server;
  std::unique_ptr<Transport> client;
  SimTransport* sim = nullptr;
  PeerId server_side = kNoPeer;  // the client as the server sees it
  PeerId client_side = kNoPeer;  // the server as the client sees it
  bool connected = false;
  std::vector<TransportEvent> at_server;
  std::vector<TransportEvent> at_client;

  void pump() {
    TransportEvent ev;
    while (server->poll(&ev)) {
      if (ev.type == TransportEvent::Type::Connected) server_side = ev.peer;
      at_server.push_back(std::move(ev));
    }
    while (client->poll(&ev)) {
      if (ev.type == TransportEvent::Type::Connected) connected = true;
      at_client.push_back(std::move(ev));
    }
    server->flush();
    client->flush();
  }
  int count(const std::vector<TransportEvent>& evs, TransportEvent::Type t,
            Delivery d = Delivery::Reliable) const {
    int n = 0;
    for (const auto& e : evs) {
      if (e.type == t && (t != TransportEvent::Type::Received || e.delivery == d)) ++n;
    }
    return n;
  }
};

bool open_ends(Ends& e, bool with_sim, Check& check) {
  e.server = make_enet_transport();
  std::string err;
  if (!check(e.server->listen(0, 4, &err), "listen on a free port: " + err)) return false;
  check(e.server->port() != 0, "an ephemeral port should be reported");
  std::unique_ptr<Transport> c = make_enet_transport();
  if (with_sim) {
    auto sim = std::make_unique<SimTransport>(std::move(c), LinkSim{});
    e.sim = sim.get();
    c = std::move(sim);
  }
  e.client = std::move(c);
  e.client_side = e.client->connect("127.0.0.1", e.server->port(), &err);
  if (!check(e.client_side != kNoPeer, "connect should start: " + err)) return false;
  return check(pump_until([&] { e.pump(); },
                          [&] { return e.connected && e.server_side != kNoPeer; }, 3.0),
               "loopback connect should complete within 3 s");
}

void enet_checks(Check& check) {
  Ends e;
  if (!open_ends(e, false, check)) return;
  check(std::string(e.server->name()).find("ENet") != std::string::npos, "backend name");
  e.client->send(e.client_side, Delivery::Reliable, bytes_of("howdy"));
  e.client->send(e.client_side, Delivery::Unreliable, bytes_of("dust"));
  // 3 KB unreliable: fragments and reassembles.
  std::vector<std::uint8_t> big(3000);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7);
  e.client->send(e.client_side, Delivery::Unreliable, big);
  const bool got = pump_until(
      [&] { e.pump(); },
      [&] {
        return e.count(e.at_server, TransportEvent::Type::Received) == 1 &&
               e.count(e.at_server, TransportEvent::Type::Received, Delivery::Unreliable) == 2;
      },
      3.0);
  if (!check(got, "reliable + unreliable messages should arrive")) return;
  bool howdy = false;
  bool big_ok = false;
  for (const auto& ev : e.at_server) {
    if (ev.type != TransportEvent::Type::Received) continue;
    if (ev.delivery == Delivery::Reliable && ev.data == bytes_of("howdy")) howdy = true;
    if (ev.delivery == Delivery::Unreliable && ev.data == big) big_ok = true;
  }
  check(howdy && big_ok, "payloads should arrive intact (3 KB unreliable included)");
  check(e.server->stats(e.server_side).bytes_received == 5 + 4 + 3000,
        "the server should count payload bytes");
  // A goodbye with a reason reaches the client.
  e.server->disconnect(e.server_side, 7);
  const bool bye = pump_until(
      [&] { e.pump(); },
      [&] { return e.count(e.at_client, TransportEvent::Type::Disconnected) == 1; }, 3.0);
  if (check(bye, "the client should hear the disconnect")) {
    for (const auto& ev : e.at_client) {
      if (ev.type == TransportEvent::Type::Disconnected) {
        check(ev.reason == 7, "the disconnect reason should travel");
      }
    }
  }
  check(!e.client->send(e.client_side, Delivery::Reliable, bytes_of("late")),
        "sending to a gone peer should fail");
  e.client->close();
  e.server->close();
}

void sim_checks(Check& check, int* rtt_ms, int* kept) {
  Ends e;
  if (!open_ends(e, true, check)) return;
  // 60 ms each way: an echo takes at least 120 ms.
  e.sim->set_sim(LinkSim{60, 0, 0.0f});
  const auto t0 = Clock::now();
  e.client->send(e.client_side, Delivery::Reliable, bytes_of("ping"));
  bool echoed = false;
  pump_until(
      [&] {
        e.pump();
        for (auto& ev : e.at_server) {
          if (ev.type == TransportEvent::Type::Received && !ev.data.empty()) {
            e.server->send(ev.peer, Delivery::Reliable, ev.data);
            ev.data.clear();
          }
        }
      },
      [&] {
        if (!echoed) echoed = e.count(e.at_client, TransportEvent::Type::Received) == 1;
        return echoed;
      },
      3.0);
  *rtt_ms = static_cast<int>(
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  check(echoed && *rtt_ms >= 115, "60 ms each way should make a >= 120 ms echo");
  // 50% loss: unreliable thins out, reliable all lands in order.
  e.sim->set_sim(LinkSim{5, 4, 0.5f});
  e.at_server.clear();
  for (int i = 0; i < 200; ++i) {
    std::vector<std::uint8_t> u = {static_cast<std::uint8_t>(i)};
    e.client->send(e.client_side, Delivery::Unreliable, u);
  }
  for (int i = 0; i < 50; ++i) {
    std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(i), 1};
    e.client->send(e.client_side, Delivery::Reliable, m);
  }
  pump_until([&] { e.pump(); },
             [&] { return e.count(e.at_server, TransportEvent::Type::Received) == 50; }, 3.0);
  pump_until([&] { e.pump(); }, [] { return false; }, 0.1);
  *kept = e.count(e.at_server, TransportEvent::Type::Received, Delivery::Unreliable);
  check(*kept > 50 && *kept < 150, "50% loss should keep roughly half of 200 unreliable");
  int next = 0;
  bool ordered = true;
  for (const auto& ev : e.at_server) {
    if (ev.type == TransportEvent::Type::Received && ev.delivery == Delivery::Reliable) {
      ordered = ordered && ev.data.size() == 2 && ev.data[0] == next;
      ++next;
    }
  }
  check(ordered && next == 50, "reliable traffic should all land, in order, through loss");
  check(e.sim->dropped() > 0, "the sim should count what it dropped");
  e.client->close();
  e.server->close();
}

}  // namespace

int run_transport_smoke() {
  Check check;
  codec_checks(check);
  enet_checks(check);
  int rtt = 0;
  int kept = 0;
  sim_checks(check, &rtt, &kept);
  if (!check.failure.empty()) {
    std::cerr << "Transport smoke failed: " << check.failure << '\n';
    return 1;
  }
  std::cout << "[smoke] transport OK (byte codec + varints + overrun refusal, host:port parsing, "
               "ENet loopback connect, reliable + unreliable + 3 KB fragmented, disconnect "
               "reason, link sim: 60 ms each way echo "
            << rtt << " ms, 50% loss kept " << kept
            << "/200 unreliable, 50/50 reliable in order)\n";
  return 0;
}

// The editor's Play with posse: server + riders in the Admin process on the
// scene the editor has open; riders saddle up and go home live, the lag sim
// reaches every rider's link, and breaking camp leaves the scene alone.
int run_editor_posse_smoke() {
  namespace fs = std::filesystem;
  using namespace ts::tombstone;
  auto fail = [](const std::string& what) {
    std::cerr << "[smoke] editor posse FAILED: " << what << '\n';
    return 1;
  };
  const fs::path dir = fs::path("TombStoneProjects") / "_posse_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  editor::ProjectInfo info;
  info.id = "_posse_smoke";
  info.name = "Posse Smoke";
  info.path = dir.string();
  editor::Editor2DScreen scr(info);
  scr.on_enter();
  const std::size_t entities = scr.workspace().entities().size();
  scr.set_posse_riders(3);
  scr.set_posse_sim(LinkSim{50, 10, 0.03f});
  if (!scr.start_posse() || !scr.posse_running()) return fail("Play with posse would not start");
  mp::LocalPosse& posse = *scr.posse();
  auto pump = [&](double seconds, const std::function<bool()>& done) {
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    auto last = Clock::now();
    while (Clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      const auto now = Clock::now();
      ts::tombstone::runtime::PlayerInput p1;
      p1.move_x = 1.0f;
      p1.connected = true;
      scr.update_posse(std::chrono::duration<double>(now - last).count(), p1);
      last = now;
      if (done && done()) return true;
    }
    return !done || done();
  };
  auto seated = [&](int n) {
    return [&posse, n] {
      if (posse.server().rider_count() != n || static_cast<int>(posse.clients().size()) != n) {
        return false;
      }
      for (const auto& c : posse.clients()) {
        if (!c->joined() || !c->has_snapshot()) return false;
      }
      return true;
    };
  };
  if (!pump(6.0, seated(3))) return fail("3 riders should be seated");
  const std::uint64_t p1 = posse.clients()[0]->rider();
  const runtime::Actor* a = posse.server().world().find(p1);
  const float x0 = a ? a->data.x : 0.0f;
  pump(0.5, nullptr);
  a = posse.server().world().find(p1);
  if (!a || a->data.x <= x0) return fail("P1 should ride the editor's keyboard input");
  if (posse.clients()[1]->link_sim().latency_ms != 50) return fail("lag sim should reach riders");
  scr.set_posse_sim(LinkSim{120, 30, 0.1f});
  for (const auto& c : posse.clients()) {
    if (c->link_sim().latency_ms != 120) return fail("lag sim changes should reach every rider");
  }
  scr.set_posse_riders(4);
  if (!pump(6.0, seated(4))) return fail("Saddle Another should seat a 4th rider");
  scr.set_posse_riders(2);
  if (!pump(6.0, seated(2))) return fail("Send One Home should free two seats");
  bool wired = false;
  for (const auto& l : scr.telegraph().lines()) {
    wired |= l.entry.channel == "posse" && l.entry.text.find("rode in as P3") != std::string::npos;
  }
  if (!wired) return fail("the Telegraph should hear riders ride in");
  const double rtt = posse.clients()[0]->stats().rtt_ms;
  scr.stop_posse();
  if (scr.posse_running() || scr.workspace().entities().size() != entities) {
    return fail("Break Camp should stop the posse and leave the scene alone");
  }
  scr.on_exit();
  fs::remove_all(dir, ec);
  std::cout << "[smoke] editor posse OK (Play with posse: 3 riders seated, P1 on the keyboard, "
               "lag sim live on every link (RTT "
            << static_cast<int>(rtt)
            << " ms), saddle up to 4 and send 2 home live, Telegraph wired, Break Camp leaves the "
               "scene untouched)\n";
  return 0;
}

int run_network_smoke() {
  if (run_transport_smoke() != 0) return 1;
  ts::tombstone::mp::PosseSmokeOptions opt;
  opt.riders = 3;
  opt.sim = LinkSim{80, 20, 0.08f};
  const ts::tombstone::mp::PosseSmokeResult r = ts::tombstone::mp::run_posse_smoke(opt);
  if (!r.ok) {
    std::cerr << "[smoke] posse FAILED: " << r.failure << '\n';
    return 1;
  }
  std::cout << "[smoke] posse OK (" << r.summary << ")\n";
  return run_editor_posse_smoke();
}
