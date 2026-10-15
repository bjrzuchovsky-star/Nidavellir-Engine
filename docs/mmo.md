# Persistent world: the Town Hall (ts_mmo)

`ts_mmo` is the MMO backbone. It runs on the same ENet transport as the posse
(`ts.posse`), but the posse is a 4-player session in which a host plays too.
The Town Hall is a **dedicated, persistent world server**: accounts, saved
riders, zones, interest management, server-checked movement, chat channels
and an admin console. It is a separate module and protocol (message types
64–75), so posse games are unchanged.

## Running a server

```
ts_server --mmo --project <dir> [--port 24650] [--db <file>] [--admin <user> <password>] [--seconds N]
ts_server --mmo-sample <dir>                      # write a sample mmo.json (town + mine)
ts_server --mmo-loadtest 200 [--connect host:port] [--project <dir>] [--seconds 10]
```

- The server runs headless. Its stdin is the **admin console**: type `help`,
  and `quit` closes the server cleanly, saving every rider first. It prints
  stats every 30 s.
- The database defaults to `<project>/mmo/world.db`. This is SQLite in WAL
  mode, with the 3.46.1 amalgamation fetched by hash at configure time.
- `--admin user pw` creates an admin account, or promotes an existing one.

### From the editor

In a 2D project with `ts.posse`, open **View > Town Hall (Persistent World)**
or **Play > Town Hall (persistent world server)...**. The panel offers:

- **Open Town Hall here**: an in-editor server on its own thread, with tabs
  for **Map**, **Riders**, **Chat** and **Console**:
  - The map shows each zone's blockers, portals, pickups, NPCs and riders.
    Click a rider to see its interest circle.
  - The riders table shows RTT, how many entities each rider sees, anti-cheat
    strikes and items, with a Kick button.
  - The chat tab shows the chat feed and an announce box.
  - The console takes the full set of admin commands.
  - A bots slider sends simulated riders in.
- **Launch ts_server --mmo**: starts the real server as its own process, in
  its own console window on Windows, for this project and port.
- **Write sample mmo.json** appears when the project has none.

## World settings: `<project>/mmo.json` (optional)

```json
{
  "tick_rate": 20, "speed": 160, "interest_radius": 640,
  "save_every": 30, "linkdead_seconds": 60, "max_players": 500,
  "open_registration": true, "argon2_kib": 4096,
  "chat": {"channels": ["trade", "posse"], "say_radius": 400, "max_length": 240, "per_10s": 8},
  "zones": [
    {"id": "town", "name": "Frontier Town", "scene": "scene.json",
     "bounds": [0, 0, 4000, 4000], "spawn": [2000, 2000],
     "blockers": [[x0, y0, x1, y1]],
     "portals": [{"rect": [3900, 1950, 4000, 2050], "to": "mine", "at": [120, 750]}],
     "items": [{"name": "gold_nugget", "at": [2060, 2000], "respawn": 20}]}
  ]
}
```

Without `mmo.json`, `scene.json` becomes the single zone `main`:

- the Player entity is the spawn point;
- solid static colliders become blockers;
- entities named `item:<name>` become pickups.

A zone's `scene` also runs its NPCs through a server-side `runtime::World`.

## Accounts and security

- Passwords are hashed with **Argon2i** (Monocypher). The stored format is
  `argon2i$m=<KiB>$t=<passes>$<salt hex>$<hash hex>`, with a 16-byte random
  salt per account and a 32-byte hash, compared in constant time
  (`crypto_verify32`).
- Usernames are case-insensitive and 3–24 characters. Passwords are 6–128
  characters.
- Failed logins lock the account for 30 s after 5 failures, and the same
  limit applies per connection.
- Bans can be timed or permanent. Mutes are timed.
- Each login hands out a fresh 256-bit **resume token**. Only its BLAKE2b
  digest is stored, in the `sessions` table.
- Note: ENet traffic is **not encrypted**. Passwords cross the wire in plain
  text. Put the server behind a VPN or tunnel for anything beyond friends and
  a LAN.

## Persistence (SQLite) and Strongbox

The database has these tables:

- `accounts`;
- `characters`: one per account, with a unique name, zone, position,
  playtime, and story JSON holding `vars` and `quests`;
- `inventory`;
- `sessions`;
- `meta`: `schema_version` = 1.

Riders are saved every `save_every` seconds in one transaction, and also on
logout, on admin `save`, and on shutdown.

**Strongbox bridge:** `strongbox export <rider> <slot>` writes
`<project>/saves/slots/<slot>.save.json`, and `strongbox import <rider> <slot>`
reads it back. Both use `story::SaveStore`, with the sections `vars`,
`quests`, `player` (position and zone) and `mmo` (inventory). A single-player
save and an MMO rider can therefore move both ways, for online or offline
riders.

## Interest management

Each zone keeps a hash grid. Each tick, a rider receives only the entities
within `interest_radius`. Entities leave the view only beyond 1.15× that
radius, so riders on the edge do not flicker.

- Entities that come into range are sent reliably (`Appear`), as are those
  that leave it (`Vanish`).
- Movement goes out in unreliable snapshots of only what moved, with a full
  refresh every second.

The load test reports how many entities each bot sees against the total
number of entities.

## Movement and anti-cheat

The client sends its intent (quantised to 1/127) together with the position
it predicted. The server runs the same `step_rider` (box collision, then
substeps, then bounds) with the zone geometry the client received, so honest
clients never need correcting.

- A claim that is more than 2 px off is corrected. The server snaps the
  rider back and the client replays its unacknowledged moves. Each
  correction is a strike.
- A claim more than half a second of travel away counts as a teleport worth
  4 strikes.
- Move floods are capped by a token bucket and a pending cap.
- Strikes decay at 0.5/s. At 10 strikes the rider is kicked with reason
  `kMmoCheat`.

## Chat

| Channel | Who hears it |
|---|---|
| `say` | Riders within `say_radius` |
| `zone` | The rider's zone |
| `world` | Everyone |
| custom channels (`/join trade`, `/leave trade`) | Riders who joined |
| `/w <name> text` | One rider (whisper) |
| `/a text` | Admins only |

Other commands: `/who`, `/where`, `/channels` and `/help`. Chat is limited to
`per_10s` lines per rider, and muted riders are not heard.

## Sessions and reconnect

When the line drops, the rider stays in the world, **link-dead**, for
`linkdead_seconds`. `MmoClient` resumes automatically, with up to 6 tries,
using its token. The server attaches the same rider, hands out a new token
and invalidates the old one. Logging in from somewhere else takes over the
rider and disconnects the old connection (reason `Elsewhere`).

## Admin console

`help, who, stats, zones, kick, ban <user> [minutes] (none = permanent), unban, mute <name> [minutes], unmute,
tp <name> <x> <y> [zone], give <name> <item> [n], take, announce <text>, save,
account create|admin|password, quest <name> start|progress|complete|fail <quest> [objective], var <name> <key> [value],
strongbox export|import <name> <slot>, shutdown [seconds]`

All of these work from the `ts_server` stdin, from the editor's Console tab,
and for admin riders in game through `/admin <command>`.

## Pickups and quests

`Interact` with a pickup within 64 px puts it in the inventory and sets
`items.<name>` in the rider's story vars. Quest objectives with the same id
progress, and quest rewards run `set`, `add`, `give` and `quest` actions.
Pickups respawn after `respawn` seconds.

## Client API

`mmo::MmoClient` is the game-side API. Its methods are:

- connecting: `login`, `create`, `resume`, `quit`;
- each frame: `update(dt)`;
- actions: `set_move`, `say`, `interact`;
- state: `entities()`, `inventory()`, `inbox()`, `x()`/`y()` (predicted) and
  `stats()`.

`ts_game` does not draw an MMO client yet. Games call this API from C++ or
plugins.

## Load test

`ts_server --mmo-loadtest N` creates bots in batches. The bots wander, chat
and pick things up, and some drop their line and resume. The test reports:
riders in the world, resumes, entities seen, corrections, cheat kicks,
server tick time and bandwidth. On the dev box, 200 bots on the sample world
ran at about 3.8 ms average per tick, with 0 corrections.

## Smoke

`ts_admin --smoke-mmo` (also part of `--smoke`) checks:

- crypto, the SQLite store, protocol fuzzing of truncated messages, and the
  zones;
- a live ENet server: logins and failures, interest appear/vanish, movement
  with 0 corrections, chat, whisper and mute, pickups, the anti-cheat kick,
  resume with the old token refused, admin commands, Strongbox
  export/import, and persistence across a restart;
- a 48-bot load test;
- the editor's Town Hall panel.

## Limits

- The Town Hall is 2D: riders move on the XY plane. 3D projects can host it,
  but zones use the 2D geometry.
- There is one server process. Zones do not shard across machines.
