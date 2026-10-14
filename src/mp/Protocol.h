#pragma once

// The TombStone multiplayer wire protocol. Every message is one ByteWriter
// buffer starting with its MsgType byte. Reliable: hello / welcome / reject
// / events / roster. Unreliable: input (client -> server, last few ticks
// repeated so a lost packet costs nothing) and snapshots (server -> client,
// newest wins).
//
// Decoders never trust the wire: they return false on any malformed or
// oversized message and the caller drops it.

#include "net/ByteStream.h"
#include "runtime/Input.h"
#include "runtime/World.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mp {

// 2: plugin channel messages.
inline constexpr std::uint16_t kProtocolVersion = 2;
inline constexpr int kMaxPosse = kMaxPlayers;  // four riders
// Inputs repeated in every input packet (newest first).
inline constexpr int kInputRedundancy = 8;
inline constexpr std::size_t kMaxNameLength = 24;
inline constexpr std::size_t kMaxSceneBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaxSnapshotActors = runtime::World::kMaxActors;

// Disconnect reasons (Transport::disconnect / TransportEvent::reason).
enum DisconnectReason : std::uint32_t {
  kReasonNone = 0,
  kReasonFull = 1,       // the posse is full (4 riders)
  kReasonShutdown = 2,   // server closing
  kReasonVersion = 3,    // protocol mismatch
  kReasonQuit = 4,       // client rode off
  kReasonBadMessage = 5, // malformed traffic
  kReasonKicked = 6,
};
const char* reason_text(std::uint32_t reason);

enum class MsgType : std::uint8_t {
  Hello = 1,
  Welcome = 2,
  Reject = 3,
  Input = 4,
  Snapshot = 5,
  Event = 6,
  Roster = 7,
  Plugin = 8,
};

struct HelloMsg {
  std::uint16_t version = kProtocolVersion;
  std::string name;
  std::uint64_t scene_hash = 0;  // client's own scene.json (0 = none)
};

struct WelcomeMsg {
  int slot = 0;
  std::uint64_t rider = 0;
  std::uint64_t tick = 0;
  int tick_rate = runtime::kTickRate;
  int snapshot_every = 2;
  std::string project_name;
  std::string scene_json;  // the server's scene, so a client needs no copy
  std::uint64_t scene_hash = 0;
};

struct RejectMsg {
  std::uint32_t reason = kReasonNone;
  std::string text;
};

struct InputCmd {
  std::uint32_t seq = 0;  // client tick number, 1-based
  runtime::PlayerInput input;
};

struct InputMsg {
  std::uint32_t client_time_ms = 0;  // echoed in snapshots (RTT)
  std::uint16_t rtt_ms = 0;          // what the client measured last
  std::vector<InputCmd> cmds;        // newest first, consecutive seqs
};

struct SnapshotMsg {
  std::uint64_t tick = 0;
  std::uint32_t ack_seq = 0;        // last input of this client applied
  std::uint32_t echo_time_ms = 0;   // newest client_time_ms seen
  std::vector<runtime::NetActor> actors;
};

struct EventMsg {
  enum class Kind : std::uint8_t { Join = 1, Leave = 2, Toast = 3, Log = 4 };
  Kind kind = Kind::Log;
  int slot = -1;
  std::uint8_t level = 0;  // runtime::LogLevel for Log
  float seconds = 0.0f;    // toast duration
  std::string channel;
  std::string text;
};

// A plugin's network channel (World::net_post / net_deliver). Reliable.
inline constexpr std::size_t kMaxPluginChannel = 32;
inline constexpr std::size_t kMaxPluginBytes = 512u * 1024u;
struct PluginMsg {
  std::string channel;
  std::vector<std::uint8_t> bytes;
};

struct RosterEntry {
  int slot = 0;
  std::string name;
  std::uint16_t rtt_ms = 0;
  std::uint64_t rider = 0;
};

struct RosterMsg {
  std::vector<RosterEntry> riders;
};

std::vector<std::uint8_t> encode(const HelloMsg& m);
std::vector<std::uint8_t> encode(const WelcomeMsg& m);
std::vector<std::uint8_t> encode(const RejectMsg& m);
std::vector<std::uint8_t> encode(const InputMsg& m);
std::vector<std::uint8_t> encode(const SnapshotMsg& m);
std::vector<std::uint8_t> encode(const EventMsg& m);
std::vector<std::uint8_t> encode(const RosterMsg& m);
std::vector<std::uint8_t> encode(const PluginMsg& m);

// Type of a raw message (false when empty / unknown).
bool peek_type(const std::vector<std::uint8_t>& data, MsgType* out);

bool decode(const std::vector<std::uint8_t>& data, HelloMsg* out);
bool decode(const std::vector<std::uint8_t>& data, WelcomeMsg* out);
bool decode(const std::vector<std::uint8_t>& data, RejectMsg* out);
bool decode(const std::vector<std::uint8_t>& data, InputMsg* out);
bool decode(const std::vector<std::uint8_t>& data, SnapshotMsg* out);
bool decode(const std::vector<std::uint8_t>& data, EventMsg* out);
bool decode(const std::vector<std::uint8_t>& data, RosterMsg* out);
bool decode(const std::vector<std::uint8_t>& data, PluginMsg* out);

// Printable, trimmed, at most kMaxNameLength; "" becomes "Stranger".
std::string clean_name(const std::string& name);

// Hash of scene.json text (handshake check).
std::uint64_t scene_hash(const std::string& scene_json);

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
