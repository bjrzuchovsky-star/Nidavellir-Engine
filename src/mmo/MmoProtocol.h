#pragma once

// Town Hall wire protocol (ts_mmo), on the same Transport / ENet as the
// posse. Every message is one ByteWriter buffer that starts with its type
// byte; types start at 64 so a posse client that knocks gets a clean
// "bad message" instead of confusion. Decoders never trust the wire.
//
// Reliable: login / login ok / login fail / zone / appear / vanish / chat /
// inventory / interact. Unreliable: move (client -> server, last few
// repeated) and snapshot (server -> client, positions of what you can see).

#include "mmo/MmoWorld.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mmo {

inline constexpr std::uint16_t kMmoProtocolVersion = 1;
inline constexpr std::uint16_t kMmoDefaultPort = 24650;
inline constexpr std::size_t kMaxUser = 24;
inline constexpr std::size_t kMaxPassword = 128;
inline constexpr std::size_t kMaxChat = 1000;
inline constexpr int kMoveRedundancy = 4;

// Entity ids on the wire: characters and pickups live in their own ranges
// above zone World actor ids.
inline constexpr std::uint64_t kCharacterBase = 1ull << 48;
inline constexpr std::uint64_t kItemBase = 2ull << 48;

// Disconnect reasons (on top of mp::DisconnectReason values).
enum MmoReason : std::uint32_t {
  kMmoQuit = 4,           // same as mp::kReasonQuit
  kMmoShutdown = 2,
  kMmoBadMessage = 5,
  kMmoKicked = 6,
  kMmoLoginFailed = 20,
  kMmoElsewhere = 21,     // the same character logged in again
  kMmoCheat = 22,         // movement checks gave up on this client
  kMmoBanned = 23,
  kMmoFull = 24,
};

enum class MmoMsg : std::uint8_t {
  Login = 64,
  LoginOk = 65,
  LoginFail = 66,
  Zone = 67,
  Move = 68,
  Snapshot = 69,
  Appear = 70,
  Vanish = 71,
  ChatSend = 72,
  Chat = 73,
  Interact = 74,
  Inventory = 75,
};

enum class LoginMode : std::uint8_t { Login = 0, Create = 1, Resume = 2 };

struct LoginMsg {
  std::uint16_t version = kMmoProtocolVersion;
  LoginMode mode = LoginMode::Login;
  std::string user;
  std::string password;
  std::string character;  // Create: the rider's name
  std::string token;      // Resume
};

struct LoginOkMsg {
  std::uint64_t entity = 0;  // your character's entity id
  std::string name;
  std::string token;         // resume token (keep it; a new one each login)
  std::uint64_t tick = 0;
  int tick_rate = 20;
  float speed = 160.0f;
  float interest_radius = 640.0f;
  bool admin = false;
  bool resumed = false;
};

struct LoginFailMsg {
  std::uint8_t code = 0;  // 1 bad credentials, 2 banned, 3 name taken, 4 full,
                          // 5 bad token, 6 throttled, 7 closed, 8 bad input
  std::string text;
};

struct ZoneMsg {
  ZoneGeometry geo;
  float x = 0.0f, y = 0.0f;  // where you stand
};

struct MoveCmd {
  std::uint32_t seq = 0;
  float mx = 0.0f, my = 0.0f;
};

struct MoveMsg {
  std::vector<MoveCmd> cmds;  // newest first, consecutive seqs
  float claim_x = 0.0f, claim_y = 0.0f;  // predicted position after cmds[0]
  std::uint32_t client_ms = 0;
  std::uint16_t rtt_ms = 0;
};

struct SnapRow {
  std::uint64_t id = 0;
  float x = 0.0f, y = 0.0f;
};

struct SnapshotMsg {
  std::uint64_t tick = 0;
  std::uint32_t ack = 0;        // last move applied
  float x = 0.0f, y = 0.0f;     // your authoritative position
  bool corrected = false;       // the server overruled your claim: snap
  std::uint32_t echo_ms = 0;
  std::vector<SnapRow> rows;    // visible entities that moved
};

enum class EntityKind : std::uint8_t { Character = 0, Actor = 1, Item = 2 };

struct AppearRow {
  std::uint64_t id = 0;
  EntityKind kind = EntityKind::Actor;
  std::string name;
  float x = 0.0f, y = 0.0f;
};

struct AppearMsg {
  std::vector<AppearRow> rows;
};

struct VanishMsg {
  std::vector<std::uint64_t> ids;
};

struct ChatSendMsg {
  std::string channel;  // say / zone / world / <custom>; text may be a /command
  std::string text;
};

struct ChatMsg {
  std::string channel;  // + "whisper", "system", "admin"
  std::string from;
  std::string text;
};

struct InteractMsg {
  std::uint64_t entity = 0;
};

struct InventoryMsg {
  std::map<std::string, int> items;
};

std::vector<std::uint8_t> encode(const LoginMsg& m);
std::vector<std::uint8_t> encode(const LoginOkMsg& m);
std::vector<std::uint8_t> encode(const LoginFailMsg& m);
std::vector<std::uint8_t> encode(const ZoneMsg& m);
std::vector<std::uint8_t> encode(const MoveMsg& m);
std::vector<std::uint8_t> encode(const SnapshotMsg& m);
std::vector<std::uint8_t> encode(const AppearMsg& m);
std::vector<std::uint8_t> encode(const VanishMsg& m);
std::vector<std::uint8_t> encode(const ChatSendMsg& m);
std::vector<std::uint8_t> encode(const ChatMsg& m);
std::vector<std::uint8_t> encode(const InteractMsg& m);
std::vector<std::uint8_t> encode(const InventoryMsg& m);

bool peek(const std::vector<std::uint8_t>& data, MmoMsg* out);
bool decode(const std::vector<std::uint8_t>& d, LoginMsg* out);
bool decode(const std::vector<std::uint8_t>& d, LoginOkMsg* out);
bool decode(const std::vector<std::uint8_t>& d, LoginFailMsg* out);
bool decode(const std::vector<std::uint8_t>& d, ZoneMsg* out);
bool decode(const std::vector<std::uint8_t>& d, MoveMsg* out);
bool decode(const std::vector<std::uint8_t>& d, SnapshotMsg* out);
bool decode(const std::vector<std::uint8_t>& d, AppearMsg* out);
bool decode(const std::vector<std::uint8_t>& d, VanishMsg* out);
bool decode(const std::vector<std::uint8_t>& d, ChatSendMsg* out);
bool decode(const std::vector<std::uint8_t>& d, ChatMsg* out);
bool decode(const std::vector<std::uint8_t>& d, InteractMsg* out);
bool decode(const std::vector<std::uint8_t>& d, InventoryMsg* out);

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
