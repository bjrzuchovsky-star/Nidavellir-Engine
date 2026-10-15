#include "mmo/MmoLoadTest.h"

#include "net/Transport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace ts {
namespace tombstone {
namespace mmo {

namespace fs = std::filesystem;

BotSwarm::BotSwarm(BotOptions opt) : opt_(std::move(opt)), rng_(opt_.seed) {}

BotSwarm::~BotSwarm() { quit_all(); }

void BotSwarm::add(int n) {
  for (int i = 0; i < n; ++i) {
    auto b = std::make_unique<Bot>();
    b->client = std::make_unique<MmoClient>();
    char name[32];
    std::snprintf(name, sizeof name, "%s%04d", opt_.prefix.c_str(), next_id_++);
    b->user = name;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    b->will_drop = u(rng_) < opt_.drop_fraction;
    b->drop_at = opt_.drop_after * (0.75 + u(rng_));
    bots_.push_back(std::move(b));
  }
}

void BotSwarm::update(double dt) {
  std::uniform_real_distribution<float> u01(0.0f, 1.0f);
  int starts = 0;
  for (auto& bp : bots_) {
    Bot& b = *bp;
    if (!b.started) {
      if (starts++ >= 8) continue;  // a few per frame: no thundering herd
      b.started = true;
      // Create the account (a second run finds it and logs in instead).
      b.client->create(opt_.host, opt_.port, b.user, opt_.password, b.user);
      b.next_chat = opt_.chat_every * (0.5 + u01(rng_));
      continue;
    }
    MmoClient& c = *b.client;
    if (c.state() == MmoClient::State::Failed && c.error().find("taken") != std::string::npos) {
      c.login(opt_.host, opt_.port, b.user, opt_.password);
    }
    c.update(dt);
    while (!c.inbox().empty()) {
      if (c.inbox().front().channel != "system") ++b.heard;
      c.inbox().pop_front();
    }
    if (!c.in_world()) continue;
    b.in_world_for += dt;
    const ZoneGeometry& z = c.zone();
    const float dx = b.tx - c.x(), dy = b.ty - c.y();
    if (dx * dx + dy * dy < 24.0f * 24.0f || (b.tx == 0.0f && b.ty == 0.0f) || u01(rng_) < 0.002f) {
      // Somewhere new within ~900 px, inside the zone, away from portals.
      std::uniform_real_distribution<float> off(-900.0f, 900.0f);
      b.tx = std::clamp(c.x() + off(rng_), z.bounds.x0 + 150.0f, z.bounds.x1 - 150.0f);
      b.ty = std::clamp(c.y() + off(rng_), z.bounds.y0 + 150.0f, z.bounds.y1 - 150.0f);
    }
    const float len = std::sqrt(dx * dx + dy * dy);
    c.set_move(len > 1 ? dx / len : 0.0f, len > 1 ? dy / len : 0.0f);
    for (const auto& [id, e] : c.entities()) {
      if (e.kind != EntityKind::Item) continue;
      const float ix = e.x - c.x(), iy = e.y - c.y();
      if (ix * ix + iy * iy < 48.0f * 48.0f) {
        c.interact(id);
        break;
      }
    }
    b.next_chat -= dt;
    if (b.next_chat <= 0.0) {
      b.next_chat = opt_.chat_every * (0.5 + u01(rng_));
      static const char* lines[] = {"howdy", "anyone seen the sheriff?", "gold by the mine",
                                    "selling horseshoes", "dust storm coming"};
      c.say(u01(rng_) < 0.7f ? "say" : "zone", lines[static_cast<int>(u01(rng_) * 4.99f)]);
    }
    if (b.will_drop && !b.dropped && b.in_world_for > b.drop_at) {
      b.dropped = true;
      c.drop_line();
    }
  }
}

void BotSwarm::quit_all() {
  for (auto& b : bots_) b->client->quit();
}

int BotSwarm::in_world() const {
  int n = 0;
  for (const auto& b : bots_) n += b->client->in_world();
  return n;
}

int BotSwarm::failed() const {
  int n = 0;
  for (const auto& b : bots_) n += b->client->state() == MmoClient::State::Failed;
  return n;
}

std::vector<std::string> BotSwarm::failures() const {
  std::vector<std::string> out;
  for (const auto& b : bots_)
    if (b->client->state() == MmoClient::State::Failed) out.push_back(b->user + ": " + b->client->error());
  return out;
}

std::uint64_t BotSwarm::resumes() const {
  std::uint64_t n = 0;
  for (const auto& b : bots_) n += b->client->stats().resumes;
  return n;
}

std::uint64_t BotSwarm::chat_heard() const {
  std::uint64_t n = 0;
  for (const auto& b : bots_) n += b->heard;
  return n;
}

std::uint64_t BotSwarm::corrections() const {
  std::uint64_t n = 0;
  for (const auto& b : bots_) n += b->client->stats().corrections;
  return n;
}

double BotSwarm::avg_visible() const {
  double s = 0;
  int n = 0;
  for (const auto& b : bots_)
    if (b->client->in_world()) {
      s += static_cast<double>(b->client->entities().size());
      ++n;
    }
  return n ? s / n : 0.0;
}

int BotSwarm::max_visible() const {
  int m = 0;
  for (const auto& b : bots_) m = std::max(m, static_cast<int>(b->client->entities().size()));
  return m;
}

std::uint64_t BotSwarm::bytes_in() const {
  std::uint64_t n = 0;
  for (const auto& b : bots_) n += b->client->stats().bytes_in;
  return n;
}

LoadTestResult run_load_test(const LoadTestOptions& opt) {
  LoadTestResult r;
  r.bots = opt.bots;
  auto say = [&](const std::string& s) {
    if (opt.progress) opt.progress(s);
  };
  std::unique_ptr<MmoServer> server;
  BotOptions bo = opt.bot;
  if (opt.connect.empty()) {
    std::error_code ec;
    if (!fs::exists(fs::path(opt.project_dir) / "mmo.json", ec)) {
      std::string err;
      if (!write_sample_world(opt.project_dir, &err)) {
        r.failure = err;
        return r;
      }
    }
    server = std::make_unique<MmoServer>();
    MmoServerConfig cfg;
    cfg.project_dir = opt.project_dir;
    cfg.db_path = opt.db_path;
    cfg.port = 0;
    cfg.argon2_kib = opt.argon2_kib;
    std::string err;
    if (!server->start(std::move(cfg), &err)) {
      r.failure = "server: " + err;
      return r;
    }
    bo.host = "127.0.0.1";
    bo.port = server->port();
  } else {
    std::string host;
    std::uint16_t port = 0;
    if (!net::parse_endpoint(opt.connect, kMmoDefaultPort, &host, &port)) {
      r.failure = "bad endpoint " + opt.connect;
      return r;
    }
    bo.host = host;
    bo.port = port;
  }
  BotSwarm swarm(bo);
  swarm.add(opt.bots);
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  auto last = t0;
  double report = 0.0;
  while (true) {
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    const double t = std::chrono::duration<double>(now - t0).count();
    if (server) server->update(dt);
    swarm.update(dt);
    if (server) server->update(0.0);
    r.peak_in_world = std::max(r.peak_in_world, swarm.in_world());
    if (t - report >= 1.0) {
      report = t;
      char line[256];
      std::snprintf(line, sizeof line, "%.0f s: %d/%d in the world, sees %.1f entities on average%s", t,
                    swarm.in_world(), opt.bots, swarm.avg_visible(),
                    server ? (", server tick " + std::to_string(server->stats().tick_ms_avg).substr(0, 5) + " ms").c_str() : "");
      say(line);
    }
    if (t >= opt.seconds) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  r.in_world = swarm.in_world();
  r.resumes = swarm.resumes();
  r.chat_heard = swarm.chat_heard();
  r.corrections = swarm.corrections();
  r.avg_visible = swarm.avg_visible();
  r.max_visible = swarm.max_visible();
  if (server) {
    const MmoStats& s = server->stats();
    r.world_entities = s.world_entities;
    r.tick_ms_avg = s.tick_ms_avg;
    r.tick_ms_max = s.tick_ms_max;
    r.kbps_out = s.kbps_out;
    r.cheat_kicks = s.cheat_kicks;
  }
  const std::vector<std::string> fails = swarm.failures();
  swarm.quit_all();
  if (server) {
    for (int i = 0; i < 10; ++i) server->update(0.05);
    server->stop();
  }
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "%d bots, %d in the world at the end (peak %d), %llu resumed, sees %.1f of %d entities on average "
                "(max %d), %llu chat lines heard, %llu corrections, %llu cheat kicks, server tick %.2f ms avg / "
                "%.2f max, %.1f KB/s out",
                r.bots, r.in_world, r.peak_in_world, static_cast<unsigned long long>(r.resumes), r.avg_visible,
                r.world_entities, r.max_visible, static_cast<unsigned long long>(r.chat_heard),
                static_cast<unsigned long long>(r.corrections), static_cast<unsigned long long>(r.cheat_kicks),
                r.tick_ms_avg, r.tick_ms_max, r.kbps_out);
  r.summary = buf;
  if (!fails.empty()) {
    r.failure = std::to_string(fails.size()) + " bot(s) failed, first: " + fails.front();
    return r;
  }
  r.ok = true;
  return r;
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
