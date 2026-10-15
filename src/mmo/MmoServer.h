#pragma once

// Town Hall: the dedicated persistent-world server (ts_mmo). Runs headless
// in ts_server --mmo, in the editor (Town Hall panel) and in the smokes.
//
// One thread owns it and calls update(). Each tick (mmo.json tick_rate,
// 20 Hz by default) it:
//   1. reads the network: logins (Argon2i passwords, session resume),
//      moves, chat, interactions;
//   2. moves every rider with its queued intents through step_rider (the
//      same code the client predicts with) and checks the client's claimed
//      position, input rate and reach (server-authoritative anti-cheat:
//      corrections, strikes, a kick past the limit);
//   3. steps every zone's World (the scene's scripted actors) and pickups;
//   4. replicates by interest: each rider hears Appear / Vanish for what
//      enters or leaves its radius (with hysteresis) and a snapshot of what
//      moved inside it;
//   5. saves dirty characters to SQLite every save_every seconds (and on
//      logout, link-dead expiry and shutdown).
//
// Riders who drop without saying goodbye stay in the world "link-dead" for
// linkdead_seconds; their resume token brings them back to the same
// character (even across a server restart, until the token expires).

#include "mmo/MmoProtocol.h"
#include "mmo/MmoStore.h"
#include "mmo/MmoWorld.h"
#include "net/Transport.h"
#include "story/Quest.h"
#include "story/Vars.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {
class World;
}
namespace mmo {

struct MmoServerConfig {
  std::string project_dir;
  std::string db_path;          // "" = <project>/mmo/world.db
  std::uint16_t port = kMmoDefaultPort;  // 0 = any free port
  std::string bind_label = "UDP";
  // Overrides for mmo.json (testing): <= 0 / empty keeps the file's value.
  std::uint32_t argon2_kib = 0;
  double linkdead_seconds = -1.0;
  double save_every = -1.0;
  // Testing: transport instead of ENet.
  std::unique_ptr<net::Transport> transport;
};

struct CharacterView {
  std::uint64_t entity = 0;
  std::int64_t id = 0;
  std::string name;
  std::string account;
  std::string zone;
  float x = 0.0f, y = 0.0f;
  bool online = false;     // false = link-dead
  bool admin = false;
  std::uint32_t rtt_ms = 0;
  int visible = 0;
  float strikes = 0.0f;
  std::uint64_t corrections = 0;
  int items = 0;
};

struct ZoneEntityView {
  std::uint64_t id = 0;
  EntityKind kind = EntityKind::Actor;
  std::string name;
  float x = 0.0f, y = 0.0f;
};

struct ChatLine {
  double time = 0.0;
  std::string channel;
  std::string from;
  std::string to;  // whisper target
  std::string zone;
  std::string text;
};

struct MmoStats {
  std::uint64_t ticks = 0;
  int online = 0;
  int linkdead = 0;
  int accounts = 0;
  int characters = 0;
  double tick_ms_avg = 0.0;
  double tick_ms_max = 0.0;
  double kbps_out = 0.0;
  double kbps_in = 0.0;
  double avg_visible = 0.0;
  int max_visible = 0;
  int world_entities = 0;     // everything that could be replicated
  std::uint64_t appears = 0;
  std::uint64_t vanishes = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t corrections = 0;
  std::uint64_t dropped_moves = 0;
  std::uint64_t cheat_kicks = 0;
  std::uint64_t logins = 0;
  std::uint64_t failed_logins = 0;
  std::uint64_t creates = 0;
  std::uint64_t resumes = 0;
  std::uint64_t chat_lines = 0;
  std::uint64_t pickups = 0;
  std::uint64_t saves = 0;
  double last_save_ms = 0.0;
};

class MmoServer {
 public:
  MmoServer();
  ~MmoServer();
  MmoServer(const MmoServer&) = delete;
  MmoServer& operator=(const MmoServer&) = delete;

  bool start(MmoServerConfig config, std::string* error = nullptr);
  // Save everyone, say goodbye (kMmoShutdown), close the database.
  void stop();
  bool running() const { return running_; }

  // Real elapsed seconds: network + 0..4 fixed ticks.
  int update(double real_seconds);
  void tick_once();

  std::uint16_t port() const;
  double now() const { return now_; }
  std::uint64_t tick() const { return stats_.ticks; }
  const WorldConfig& world_config() const { return cfg_; }
  const MmoStats& stats() const { return stats_; }
  Store& store() { return store_; }
  bool shutdown_requested() const { return shutdown_at_ >= 0.0 && now_ >= shutdown_at_; }

  // Admin console (ts_server stdin, the editor's Town Hall, /admin in chat
  // for admin accounts). Returns the output text.
  std::string admin(const std::string& line, const std::string& by = "console");
  std::vector<std::string> admin_commands() const;

  // Accounts without a client (admin console, tools, the load test).
  bool create_account(const std::string& user, const std::string& password, bool admin,
                      const std::string& character, std::string* error);

  std::vector<CharacterView> characters() const;
  std::vector<ZoneEntityView> zone_entities(const std::string& zone) const;
  std::vector<std::string> zone_ids() const;
  const ZoneDef* zone_def(const std::string& id) const { return cfg_.zone(id); }
  const std::deque<ChatLine>& chat_log() const { return chat_log_; }
  std::vector<std::string> take_log();  // server happenings since last take
  // Save every online / link-dead character now. Returns how many.
  int save_all();

  // Strongbox bridge (MmoBridge.cpp): a character <-> a save slot of the
  // project (<project>/saves/slots/<slot>.save.json).
  bool export_to_strongbox(const std::string& character, const std::string& slot, std::string* error);
  bool import_from_strongbox(const std::string& character, const std::string& slot, std::string* error);

 private:
  struct Character;
  struct ZoneRt;
  struct PeerRt {
    net::PeerId id = net::kNoPeer;
    std::int64_t character = 0;
    int failed = 0;
    double connected_at = 0.0;
  };

  void handle(const net::TransportEvent& ev);
  void on_login(PeerRt& p, const LoginMsg& m);
  void login_fail(PeerRt& p, std::uint8_t code, const std::string& text, bool count = true);
  Character* attach(PeerRt& p, const CharacterRow& row, const AccountRow& acc, bool resumed);
  void on_move(Character& c, const MoveMsg& m);
  void on_chat(Character& c, const ChatSendMsg& m);
  void on_interact(Character& c, const InteractMsg& m);
  void chat_command(Character& c, const std::string& text);
  void deliver_chat(const std::string& channel, const std::string& from, const std::string& text,
                    const Character* origin, const std::string& to = {});
  void system_msg(Character& c, const std::string& text, const std::string& channel = "system");
  void disconnect_character(Character& c, std::uint32_t reason);
  void logout(std::int64_t id);
  void go_linkdead(Character& c);
  void move_riders(double dt);
  void step_zones(double dt);
  void replicate();
  void send_zone(Character& c);
  void send_inventory(Character& c);
  void change_zone(Character& c, const std::string& zone, float x, float y);
  void strike(Character& c, float amount, const std::string& why);
  void measure(double dt);
  CharacterRow row_of(const Character& c) const;
  Character* find_character(const std::string& name);
  ZoneRt* zone_rt(const std::string& id);
  void log(std::string line);
  void send(net::PeerId peer, net::Delivery d, const std::vector<std::uint8_t>& msg);
  void register_admin();
  void apply_story_action(Character& c, const std::string& action);

  MmoServerConfig config_;
  WorldConfig cfg_;
  Store store_;
  std::unique_ptr<net::Transport> transport_;
  bool running_ = false;
  double now_ = 0.0;
  double accumulator_ = 0.0;
  double save_timer_ = 0.0;
  double shutdown_at_ = -1.0;
  std::map<net::PeerId, PeerRt> peers_;
  std::map<std::int64_t, std::unique_ptr<Character>> chars_;
  std::vector<std::unique_ptr<ZoneRt>> zones_;
  story::QuestLog quest_template_;
  struct Lock {
    int fails = 0;
    double until = 0.0;
  };
  std::unordered_map<std::string, Lock> locks_;
  std::deque<ChatLine> chat_log_;
  std::vector<std::string> log_;
  MmoStats stats_;
  struct Cmd {
    std::string help;
    std::function<std::string(const std::vector<std::string>&, const std::string& by)> fn;
  };
  std::map<std::string, Cmd> cmds_;
  // measurement
  double measure_window_ = 0.0;
  double tick_ms_sum_ = 0.0;
  int tick_ms_n_ = 0;
  std::uint64_t bytes_out_ = 0, bytes_in_ = 0;
};

// Rider names and usernames: 3-24 of letters, digits, '_' and '-'.
bool valid_username(const std::string& s);
std::string clean_chat(const std::string& s, std::size_t max_len);
// Intent as it travels (1/127 steps): the client predicts with this.
float quantize_intent(float v);

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
