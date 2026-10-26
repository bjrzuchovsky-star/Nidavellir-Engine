# Live Team Claims (design, 0.2.0)

> Status: **stage 1 done** on `feature/team-claims` (ready for review for 0.2.0, not merged).
>
> | Stage | What | Ships in |
> |---|---|---|
> | 1 | Basic team claims: live op sync, presence / cursors / selection outlines, soft locks with take-over, mid-drag preview, terrain sculpt / paint sync, files and deletes via the Vault, late-join snapshot, sessions that survive a restart, Team menu (Host / Join / Users) | **0.2.0** |
> | 2 | Chat, roles, follow camera, play-test together | **1.0.0** |
> | 3 | Conflict merge dialog, change history with rollback, LAN host hand-over | **1.0.0** |
>
> Stages 2 and 3 are designed here and not built yet.

A *team claim* is a claim (project) that several people work on at once, live, the way a posse works a gold claim together. Everyone sees the others ride in: who is in which scene, where their cursor is, what they have selected, and every edit lands on every screen within a few hundred milliseconds.

One self-hosted server does both jobs: `ts_vault` already keeps the asset library; it now also runs **team sessions** on the next port up. For a LAN party you can **Host from my editor** instead, no server needed.

---

## Riding together (how to use stage 1)

**On a LAN, no server:** open the claim, **Team > Host from my editor...**, pick a UDP port (default 24650) and an optional passcode, **Open the claim**. Everyone else opens any claim of the same kind (2D or 3D), **Team > Join a posse...**, types the host PC's address (`192.168.1.20` or `192.168.1.20:24650`) and the passcode, **Ride out**. Joining swaps their open scene for the posse's (their scene.json is autosaved with every edit from then on).

**Through your Vault server:** run it with team sessions on:

```
ts_vault --root /srv/ts_vault --host 0.0.0.0 --port 8740 --team          # team UDP 8741
ts_vault --root /srv/ts_vault --team-port 9000                             # pick the UDP port
```

`--host 0.0.0.0` matters: riders' files come over the HTTP port (TCP 8740 here, 25565 if that is your `--port`), so open it as well as the UDP team port. A Vault on `127.0.0.1` lets other PCs join but not get files, and their Team panel says so.

The first rider joins with their Vault API key and their claim's id, which opens the session from their scene; the rest join with `*` (whatever claim the server runs, when it runs one) or the same id. The key's name is the rider's name unless they type one. Read-only keys ride as viewers. `GET /api/v1/team` (with a key) lists the open sessions. Team sessions listen on every interface; open the UDP port in the firewall only where you need it.

**While riding:** the **Team** panel lists the posse with colours, the scene each is in and how many locks each holds. Selecting an entity locks it for you; others see a coloured `[locked: name]` badge in the Hierarchy and your outline in the viewport, and an edit they make to it is turned back ("Edit undone: Locked by Annie"). Remote cursors show as coloured arrows with names. Ctrl+Z undoes only your own steps. **Leave** (or closing the editor) releases your locks at once; a crashed editor's locks go after 15 s.

- **Mid-drag preview:** while you drag, rotate, scale or nudge something you hold, the posse sees it move live (about 20 times a second on the unreliable channel). The real edit still lands once, when you let go; Esc / right-click puts it back for everyone.
- **Take over:** if someone has left a thing selected and walked off, the panel shows **Take over (n)** while you have it selected. Their lock passes to you once they have not touched it for **15 s**; while they are still working on it the server says so ("Annie is still working on it...") and nothing changes.
- **Terrain:** sculpt, smooth, flatten, paint and foliage strokes travel as one compact patch per stroke (only the touched rectangle, base64 in the op), checked against the terrain entity's lock.
- **Files:** riding in brings the posse's files with you, with nothing extra to type: every file under `assets/` and its `.meta` (`.tombstone/meta/assets/...`, so GUIDs and import settings match) travel by SHA-256 on a worker thread, and whatever you import, drop in, pull from the Vault or place goes to everyone. The panel says where files travel ("through the Vault at http://...", "over the team session"), shows a progress bar, and the editor keeps running; sprites, models and the Asset Browser pick up files as they land. Deleting a file deletes it for everyone (into `.tombstone/team/trash/`, never straight off the disk). Details and the error messages in section 4.
- **Viewers** (read-only Vault keys) show in the posse with their cursor and selection, but take no locks and send no edits.
- **Losing the trail:** if the connection drops, the editor keeps working and rides back in by itself (1 s, 2 s, 4 s ... up to every 15 s): "Lost the trail: riding back in". When it is back it is the same rider (same colour, same id range) and whatever you changed while away is merged on top of the posse's scene as one **Edits while away** step. A server restart works the same way, because the server keeps every claim's session on disk.
- **Editor restart:** the panel remembers your last ride in `<claim>/.tombstone/team/last.json` (address, port, name, claim, a rejoin token; never your key or passcode) and offers **Ride back in**. Hosting again from the same claim picks up its saved session.

**Checks:** `ts_admin --smoke-team` (in `--smoke` too) and `ts_vault --smoke`. `ts_admin --team-shot out.png` renders two editors riding together (docs screenshot).

---

## 1. Goals and non-goals

**Stage 1 goals**

- Real-time edits for 2D (scene.json v13) and 3D / VR (Scene3D v18) claims, every editor tool included, because we sync what the undo system commits rather than each tool.
- Server-authoritative ordering: every committed edit gets a sequence number; clients apply their own edits straight away (optimistic) and reconcile when the server answers.
- Late joiners get a snapshot plus the op tail and end up byte-identical with everyone else.
- Soft locks per entity / asset while someone has it selected, is dragging it or has it in the Inspector. Released on deselect, disconnect or timeout.
- Presence: user list with colours, the scene each person is in, remote cursors in the viewport, remote selection outlines, lock badges in the Hierarchy.
- Assets (and their `.meta`) travel by SHA-256 through the Vault, or over the session for a LAN host, never inline in the op stream.
- Version guard: the session records the engine version and scene formats; a mismatched editor is refused (format) or warned (engine patch level).
- `ts_admin --smoke-team`: a server and two headless editors in one process making conflicting edits; asserts convergence, lock behaviour and lock release on disconnect.

**Not in stage 1 (0.2.0):** chat, roles beyond read-only keys, follow camera, play-testing together (stage 2, 1.0.0); a conflict merge dialog, history rollback and LAN host hand-over (stage 3, 1.0.0).

---

## 2. Where edits come from: the undo system

Both editors already make **exactly one undo step per user action**: one per drag, per Inspector field change (coalesced while the field is focused), per nudge burst, per brush stroke. They are *snapshot based* (a step stores the scene before the action), not command objects, so a step has no "move entity 7 by (3,0)" payload to send.

So a team op is the **entity-level difference a committed step made**:

| Op | Payload | Meaning |
|---|---|---|
| `put` | `id`, `e` (the entity's scene.json object) | create or replace one entity |
| `del` | `id` | remove one entity |
| `order` | `ids` | the entity order (Hierarchy / draw order) after a reorder |
| `set` | `key`, `v` | a scene-level block: 2D `tile_solidity`, `lighting`; 3D `environment`, `physics` |
| `asset` | `path`, `sha256`, `size` | a project file changed; fetch that blob from the Vault (empty `sha256` = the file was deleted) |
| `terrain` | `path`, `id`, `v` {`x`,`y`,`w`,`h` or `full`, heights / splat base64, foliage} | one stroke's rectangle of a terrain's height and splat data (3D) |

An **op batch** (`commit`) is one undo step: a label ("Move 3", "Inspector: Speed"), the scene it applies to and its ops. The entity payload is the same JSON the scene file writes (`scene_json::entity_to_json` / `scene3d::entity_to_json`), so the wire format is versioned with the scene format and every component, present or future, syncs without team code knowing about it.

Why entity granularity: it is what locks protect, it makes concurrent edits to different entities commute, and "last committed write wins" per entity is easy to reason about. A per-field merge (two people editing different fields of one entity) is not needed while locks keep one editor per entity.

**Detecting a step.** Each frame, when the editor is idle (no drag, no open Inspector edit, no gizmo or brush stroke), the team binding compares the live entities with its *shadow* (the state it last synced) using the components' `operator==`, and serialises only what changed. This also catches the few edits that bypass history (prefab sync, lighting) and keeps "one op batch per drag" for free, because a drag is never idle until it is committed.

**Undo stays personal.** Ctrl+Z restores a snapshot, which would also roll back other people's work. So when a remote batch is applied, the binding *rebases* every snapshot in the local undo and redo stacks: the same `put` / `del` is applied to each stored scene. Undo then only reverts your own change, and the revert is itself an ordinary op batch everybody receives.

**Ids.** Two people creating an entity at the same moment must not both make id 42. On join the server grants each member an **id range** (2^20 ids, below 2^53 so they survive JSON). `Workspace2D` and `Scene3D` allocate new ids inside the range while one is set; outside a session nothing changes.

---

## 3. The session host

### 3.1 In ts_vault (team server)

`ts_vault --team` (default on with `--team-port`, default HTTP port + 1, i.e. 8741 UDP) runs a `team::TeamServer` beside the HTTP API:

- Transport: the engine's `net::Transport` over **ENet** (UDP, reliable ordered channel 0 for ops and locks, unreliable channel 1 for presence). The same code the multiplayer layer uses, tested and NAT-friendly; no WebSocket stack to maintain.
- Auth: the `hello` carries a Vault **API key**; the server checks it with `VaultStore::check_key` (hash compare, never logged). The key name becomes the default display name. Read-only keys join as viewers (see edits, cannot send them).
- Discovery: `GET /api/v1/team` (auth) lists open sessions (claim, scene formats, members) and the team port, so the editor's Join dialog can be filled from the Vault URL already in Settings.
- One process, one folder: sessions live in memory, keyed by the claim's GUID, and the op log is compacted into snapshots (3.4). Each session is also written to `<vault root>/team/<claim>.team.json` about a second after it changes (and on shutdown): the scene docs, `seq`, the next id range and the riders with their rejoin tokens. On start the server loads them, so a restart keeps the posse's scene and everyone rides back in as themselves. This is the "claim checkpoint" in the Vault; clients still autosave their own scene.json.

### 3.2 Host from my editor (LAN)

**Team > Host** starts the same `TeamServer` inside the editor on a UDP port (default 24650), seeded from the open scene, and joins it as the first member. Optional session passcode instead of API keys. Others **Team > Join** with `host:port`. The session is saved in `<claim>/.tombstone/team/host/`, so hosting the same claim again continues it. If the host leaves, the session ends for now (stage 3: hand-over); the host's own editor does not try to reconnect to itself.

### 3.3 Protocol (proto 1)

One JSON object per ENet packet (`t` = type). Small, readable, versioned by `proto`.

| Dir | `t` | Fields |
|---|---|---|
| C→S | `hello` | `proto`, `claim`, `claim_name`, `user`, `key` or `pass`, `engine`, `formats` {`scene2d`, `scene3d`}, `dim`, `scene`, `seed` (initial docs, only used if the session is new), `rejoin` (token from an earlier welcome, optional) |
| S→C | `welcome` | `member`, `colour`, `session` {claim, engine, formats, `files` (where files travel, section 4)}, `ids` {lo, hi}, `snap_seq`, `docs`, `tail` (commits after the snapshot), `members`, `locks`, `warn`, `rejoin` (token to keep), `back` (true when the token was known and the old id / colour restored) |
| S→C | `refuse` | `reason` (version, auth, full) |
| C→S | `op` | `cid` (client batch id), `scene`, `label`, `ops` |
| S→* | `commit` | `seq`, `from`, `cid`, `scene`, `label`, `ops` (to everyone, sender included = the ack) |
| S→C | `reject` | `cid`, `reason`, `by` (lock owner) |
| C→S | `lock` | `keys`: the full set this member wants right now |
| S→* | `locks` | the whole lock table `{key: member}` when it changes |
| C→S | `take` | `keys`: take these locks over from an idle holder |
| S→C | `lock_denied` | `keys`, `by`, `reason` (the holder is still working on it) |
| S→* | `taken` | `keys`, `from`, `to` (a lock changed hands) |
| C→S | `preview` / `preview_end` | unreliable, ~20 Hz while dragging: `scene`, `ops` (puts of entities the sender holds) / `ids`; relayed with `member`, never stored |
| C→S | `pres` | unreliable, ~10 Hz: `scene`, `cursor` [x,y,z], `sel`, `cam` |
| S→* | `pres` | the same plus `member` (relayed to the others) |
| S→* | `member` / `left` | someone joined (name, colour) / left |
| C→S | `blob_has` / `blob_put` / `blob_get` | files over the session (section 4): `rid`, `shas` / `sha`, `size`, `off`, `data` (base64) |
| S→C | `blob_r` | the reply: `rid`, `ok`, `error`, `missing` / `done` / `size`, `data` |

### 3.4 Ordering, optimism and reconciling

- The server applies batches in arrival order, stamps `seq` (1, 2, 3 ...) and broadcasts. That order is the truth.
- A client applies its own batch immediately, keeps it in `pending`, and keeps `confirmed` (the server state up to the last seq it has seen).
- A remote commit is applied to `confirmed`; to the live scene it is applied **except** for entities a pending local batch touches (that local batch has a higher seq, so it will win anyway).
- A commit with our `cid` is the ack: pop it from `pending`.
- A `reject` drops the pending batch and restores those entities from `confirmed` + the remaining pending batches.
- Safety net: whenever a client is idle with nothing pending, it compares its scene with `confirmed` (once a second) and quietly repairs any drift.
- Late join: the server keeps a snapshot at `snap_seq` and the commits after it (compacted every 256 commits). The joiner loads the snapshot, applies the tail, and is at the current seq.

### 3.5 Soft locks

- Lock keys: `e:<scene>:<entity id>` and `a:<asset path>`.
- A client asks for the entities it has selected (and what the Inspector shows), which covers dragging and inspector focus. The server grants keys that are free or already the member's, denies keys held by others, and releases keys the member no longer asks for.
- A batch touching an entity locked by someone else is **rejected** (the reject names the holder: "Locked by Mia"). Free entities are first come, first served.
- Locks die with their holder: on disconnect at once; on silence after 15 s (ENet's own timeout usually beats it).
- **Take-over:** each lock remembers when its holder last used it (a commit or preview touching it). `take` hands it over once it has been idle for 15 s (`ServerOptions::lock_idle_s`), otherwise `lock_denied`. The old holder's editor gets a note and stops asking for it; when a lock it still wants comes free it asks again.
- Viewers never hold locks.
- Shown as a coloured padlock and the holder's name in the Hierarchy, and a coloured outline in the viewport.

### 3.6 Presence

Server-assigned colours from the Dusk palette (sunset orange, sage, dusty blue, brass, rose, sky, ...). Each member reports the scene they are in, the viewport cursor (2D world x,y or 3D point under the mouse) and the selection. Remote cursors are a coloured arrow with the name; remote selections get an outline in that member's colour. The **Team** panel lists users with colour, scene, role and lock count.

---

## 4. Project files (assets and their .meta)

Scene ops reference assets by project path, so a rider needs the files as well as the scene. The files a session carries: everything under `assets/` and each asset's `.meta` under `.tombstone/meta/assets/` (the GUID, kind and import settings; without it a joiner would mint fresh GUIDs and default settings).

**How it works.** A worker thread scans those two folders every 2 s (mtime + size decide whether to re-hash). A file that is new or changed here is sent by SHA-256 to the session's file store and announced with an `asset` op `{path, sha256, size}`; every other rider whose copy differs fetches the blob (verified SHA-256) and writes it in place. A deleted file travels as an `asset` op with no hash; the others move their copy into `.tombstone/team/trash/`. On joining, the session's files win. A `.meta` that never matched the session (the joiner's asset scan made one for a file that just arrived) is replaced by the posse's, and one for a file that came from the session waits for its author's, so GUIDs stay the same everywhere. A pass fetches `.meta` files before their assets. When files land, the editor drops its cached textures / models for them and rescans the Asset Browser, so sprites and models that use them show up straight away. Big files never stall the editor; the Team panel shows files and MB done.

**Where files travel.** Nobody types a URL. The host says where in the `welcome` (`session.files`):

| Host | `session.files` | Riders use |
|---|---|---|
| `ts_vault --team` | `{"via": "vault", "port": <HTTP port>}` (+ `"loopback": true` when it listens on 127.0.0.1) | the Vault's HTTP API at the address they dialled, `http://<that host>:<port>`, with their Vault key (`POST /api/v1/uploads`, `GET /api/v1/blobs/<sha>`) |
| Host from my editor, Vault URL typed | `{"via": "vault", "url": ...}` (a 127.0.0.1 URL is swapped for the host's address) | that Vault, with their own key (the Team panel's key box, else Settings > Vault > API key) |
| Host from my editor, no Vault | `{"via": "session"}` | the team session itself: `blob_has` / `blob_put` / `blob_get` requests (48 KB chunks, base64, reliable channel) answered by the host's editor from `<claim>/.tombstone/team/host/blobs/<aa>/<sha256>` |
| older host (TombStone 0.2.0) | nothing | the Team panel's **Vault URL (optional)** if typed, else Settings > Vault > URL, else an error |

A URL typed in the Team panel always wins (handy for a Vault behind a proxy with its own URL).

**When files can't travel**, the Team panel says why in red, and the note goes to the Telegraph:

- No key for the Vault: "Files can't travel: the posse's files live in the Vault at ..., which needs your Vault API key. Put it in the Team panel's key box (or Settings > Vault > API key), then ride out and in again."
- Vault on 127.0.0.1 with a rider on another PC: "... restart ts_vault with --host 0.0.0.0 and allow TCP <port> through its firewall."
- Older host, no URL: "Files can't travel: this team host doesn't say where the posse's files live (it runs an older TombStone). Type its Vault URL in the Team panel ..."
- Vault or host unreachable, key refused: "Files can't travel through the Vault at ...: <the error>" (cleared by the next pass that works).
- **Read-only key:** the panel says "your key is read-only, so you see the posse's files but yours stay on this PC"; each local file not sent gets a note once ("Read-only key: assets/cactus.png stays on this PC"), and an object you place is put back ("Read-only key: your edit was put back"). Session blobs refuse a read-only rider's upload with "Your key is read-only: you can see the posse's files but not place your own."

---

## 5. Version guard

The session records the creator's engine version and scene formats (`scene2d` = 13, `scene3d` = 18 in 0.2.0).

- Different scene format: **refused** ("This claim's posse rides Scene3D v19; this editor writes v18. Update TombStone to join.") because entity JSON could lose fields.
- Different engine version, same formats: allowed with a **warning** in the Team panel.
- Different `proto`: refused.

---

## 6. Editor UI

- **Team** menu in both editors: Host..., Join..., Users, Leave.
- **Team** panel: status, session address, the user list (colour, name, scene, locks), host / join forms (address, API key or passcode, display name), a "Your colour" swatch.
- Hierarchy: padlock + name on entities others hold.
- Viewport: remote cursors and selection outlines; editing a locked entity shows a toast and is undone by the reject.

---

## 7. Known limits (stage 1)

- **Merging after a dropped connection is entity-level, last write wins.** Your edits while away go on top of the posse's. If someone else changed the same entity meanwhile, yours wins; if they deleted it and you changed it, it comes back. Stage 3 adds a merge dialog for these.
- **Terrain undo is a snapshot of the whole terrain.** Ctrl+Z after a stroke puts back the terrain as it was before it, which can undo another rider's stroke on the same terrain made in between. Lock the terrain (select it) while sculpting to avoid that.
- Terrain patches are the touched rectangle, uncompressed; a stroke across the whole of a 1025 x 1025 terrain is a large op (several MB). Fine on a LAN, slow over the internet.
- Your key and passcode are not saved, so after an editor restart you type them again before **Ride back in**.
- The LAN host's editor does not reconnect to itself; when it closes, the session pauses until it hosts again.
- Remote batches wait while you are in the middle of a drag or Inspector edit and land when it ends (the drag preview shows them moving in the meantime).
- Deleted files go to `.tombstone/team/trash/` and are never emptied automatically.
- Files over the team session (LAN host without a Vault) are kept by the host's editor in `.tombstone/team/host/blobs/` and never pruned; they move at LAN speed in 48 KB steps, fine for textures and models, slow for gigabytes. Use a Vault for big claims.
- Only `assets/` and its `.meta` files travel. Scripts, prefabs and other files outside `assets/` stay local unless they are under it.
- Two riders changing the same file at once: the last one sent wins (no file locks yet).

## 8. Stage 2 (1.0.0; designed, not built)

- **Chat**: `chat` messages relayed and kept in the session (last 200), with `@name` mentions; a Telegraph-style panel.
- **Roles**: owner / editor / viewer per session. The owner is the creator (or a key flagged `--team-owner`); owners can promote, demote and kick. Viewers' batches are rejected server-side; the role travels in `welcome` and `member`.
- **Follow camera**: click a user to follow their editor camera (they publish `cam` in presence at 10 Hz; the follower lerps).
- **Play-test together**: the host presses Play for the posse; the session spins up a `GameServer` (ts_mp) on the current confirmed scene and every member joins as a client; Stop returns everyone to editing. Edits are frozen while the posse rides.

## 9. Stage 3 (1.0.0; designed, not built)

- **Conflict merge**: stage 1 already rides back in after a drop and merges edits made while away (last write wins per entity). Stage 3 shows the entities both sides changed in a merge dialog (theirs / mine / both as a copy) instead.
- **Change history with rollback**: the server persists the commit log in SQLite next to `vault.db` (seq, author, label, time, ops, inverse ops). A History panel lists commits; **Roll back** commits the inverse ops as a new batch (history is never rewritten). Snapshots every N commits keep join fast.
- Host hand-over for LAN sessions. (Persistence across server restarts shipped in stage 1.)

## 10. Decided for 0.2.0

1. Sessions are keyed by the **claim GUID** (stable across renames). Vault item ids can map onto it later.
2. Locks follow the selection, and anyone can **take over** a lock idle for 15 s. Owner-only stealing waits for roles (stage 2).
3. The server **writes the session to the Vault** (`<vault root>/team/`) and clients keep autosaving their scene.json.
4. Team port: HTTP port + 1 for `ts_vault` (8741/UDP, `--team-port` to change); **24650/UDP** for hosting from the editor.
5. Viewers show presence (cursor, selection outline) and take **no locks**.
