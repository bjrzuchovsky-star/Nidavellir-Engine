# Live Team Claims (design, 0.2.0)

> Status: **stage 1 in progress** on `feature/team-claims`.
>
> | Stage | What | Ships in |
> |---|---|---|
> | 1 | Basic team claims: live op sync, presence / cursors / selection outlines, soft locks, late-join snapshot, Team menu (Host / Join / Users) | **0.2.0** |
> | 2 | Chat, roles, follow camera, play-test together | **1.0.0** |
> | 3 | Offline edits + merge, change history with rollback | **1.0.0** |
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

The first rider joins with their Vault API key and their claim's id, which opens the session from their scene; the rest join with `*` (whatever claim the server runs, when it runs one) or the same id. The key's name is the rider's name unless they type one. Read-only keys ride as viewers. `GET /api/v1/team` (with a key) lists the open sessions. Team sessions listen on every interface; open the UDP port in the firewall only where you need it.

**While riding:** the **Team** panel lists the posse with colours, the scene each is in and how many locks each holds. Selecting an entity locks it for you; others see a coloured `[locked: name]` badge in the Hierarchy and your outline in the viewport, and an edit they make to it is turned back ("Edit undone: Locked by Annie"). Remote cursors show as coloured arrows with names. Ctrl+Z undoes only your own steps. **Leave** (or closing the editor) releases your locks at once; a crashed editor's locks go after 15 s.

**Checks:** `ts_admin --smoke-team` (in `--smoke` too) and `ts_vault --smoke`. `ts_admin --team-shot out.png` renders two editors riding together (docs screenshot).

---

## 1. Goals and non-goals

**Stage 1 goals**

- Real-time edits for 2D (scene.json v11) and 3D / VR (Scene3D v16) claims, every editor tool included, because we sync what the undo system commits rather than each tool.
- Server-authoritative ordering: every committed edit gets a sequence number; clients apply their own edits straight away (optimistic) and reconcile when the server answers.
- Late joiners get a snapshot plus the op tail and end up byte-identical with everyone else.
- Soft locks per entity / asset while someone has it selected, is dragging it or has it in the Inspector. Released on deselect, disconnect or timeout.
- Presence: user list with colours, the scene each person is in, remote cursors in the viewport, remote selection outlines, lock badges in the Hierarchy.
- Assets travel through the Vault by SHA-256, never inline in the op stream.
- Version guard: the session records the engine version and scene formats; a mismatched editor is refused (format) or warned (engine patch level).
- `ts_admin --smoke-team`: a server and two headless editors in one process making conflicting edits; asserts convergence, lock behaviour and lock release on disconnect.

**Not in stage 1 (0.2.0):** chat, roles beyond read-only keys, follow camera, play-testing together (stage 2, 1.0.0); offline edits, merge and history rollback (stage 3, 1.0.0); syncing terrain heightfield *data* files live (they go through the Vault as assets, see 7).

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
| `asset` | `path`, `sha256`, `size` | a project file changed; fetch that blob from the Vault |

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
- One process, one folder: sessions live in memory; the op log is compacted into snapshots (3.4). Persisting a session across a server restart is stage 3.

### 3.2 Host from my editor (LAN)

**Team > Host** starts the same `TeamServer` inside the editor on a UDP port (default 24650), seeded from the open scene, and joins it as the first member. Optional session passcode instead of API keys. Others **Team > Join** with `host:port`. If the host leaves, the session ends (stage 3: hand-over).

### 3.3 Protocol (proto 1)

One JSON object per ENet packet (`t` = type). Small, readable, versioned by `proto`.

| Dir | `t` | Fields |
|---|---|---|
| C→S | `hello` | `proto`, `claim`, `claim_name`, `user`, `key` or `pass`, `engine`, `formats` {`scene2d`, `scene3d`}, `dim`, `scene`, `seed` (initial docs, only used if the session is new) |
| S→C | `welcome` | `member`, `colour`, `session` {claim, engine, formats}, `ids` {lo, hi}, `snap_seq`, `docs`, `tail` (commits after the snapshot), `members`, `locks`, `warn` |
| S→C | `refuse` | `reason` (version, auth, full) |
| C→S | `op` | `cid` (client batch id), `scene`, `label`, `ops` |
| S→* | `commit` | `seq`, `from`, `cid`, `scene`, `label`, `ops` (to everyone, sender included = the ack) |
| S→C | `reject` | `cid`, `reason`, `by` (lock owner) |
| C→S | `lock` | `keys`: the full set this member wants right now |
| S→* | `locks` | the whole lock table `{key: member}` when it changes |
| C→S | `pres` | unreliable, ~10 Hz: `scene`, `cursor` [x,y,z], `sel`, `cam` |
| S→* | `pres` | the same plus `member` (relayed to the others) |
| S→* | `member` / `left` | someone joined (name, colour) / left |

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
- Shown as a coloured padlock and the holder's name in the Hierarchy, and a coloured outline in the viewport.

### 3.6 Presence

Server-assigned colours from the Dusk palette (sunset orange, sage, dusty blue, brass, rose, sky, ...). Each member reports the scene they are in, the viewport cursor (2D world x,y or 3D point under the mouse) and the selection. Remote cursors are a coloured arrow with the name; remote selections get an outline in that member's colour. The **Team** panel lists users with colour, scene, role and lock count.

---

## 4. Assets via the Vault

Scene ops reference assets by project path. When someone imports or changes a file during a session, the editor sends the blob to the Vault (resumable upload, `POST /api/v1/uploads`, content-addressed) and commits an `asset` op `{path, sha256, size}`. Every other member whose file hash differs downloads `GET /api/v1/blobs/<sha>` (verified SHA-256) and writes it in place; the asset database then rescans as usual. Nothing large goes over the op channel, and a repeated file is stored once. LAN sessions without a Vault fall back to a local-library folder share, or skip with a warning.

---

## 5. Version guard

The session records the creator's engine version and scene formats (`scene2d` = 11, `scene3d` = 16).

- Different scene format: **refused** ("This claim's posse rides Scene3D v17; this editor writes v16. Update TombStone to join.") because entity JSON could lose fields.
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

- Terrain heightfield / splat **data files** change outside scene.json; they sync as `asset` ops after each stroke (stage 1 sends them when the Vault is configured), not live per brush dab.
- Mid-drag movement is only seen when the drag is committed (stage 2 may stream a transient preview on the unreliable channel).
- Sessions are in memory; a server restart ends them (everyone keeps their local scene.json, which is always autosaved).
- Remote batches wait while you are in the middle of a drag or Inspector edit and land when it ends.

---

## 8. Stage 2 (1.0.0; designed, not built)

- **Chat**: `chat` messages relayed and kept in the session (last 200), with `@name` mentions; a Telegraph-style panel.
- **Roles**: owner / editor / viewer per session. The owner is the creator (or a key flagged `--team-owner`); owners can promote, demote and kick. Viewers' batches are rejected server-side; the role travels in `welcome` and `member`.
- **Follow camera**: click a user to follow their editor camera (they publish `cam` in presence at 10 Hz; the follower lerps).
- **Play-test together**: the host presses Play for the posse; the session spins up a `GameServer` (ts_mp) on the current confirmed scene and every member joins as a client; Stop returns everyone to editing. Edits are frozen while the posse rides.

## 9. Stage 3 (1.0.0; designed, not built)

- **Offline edits + merge**: the client journals its batches with the last seq it saw. On reconnect the server replays them on top of the current state: entities untouched since are applied; entities both sides changed are conflicts, shown in a merge dialog (theirs / mine / both as a copy).
- **Change history with rollback**: the server persists the commit log in SQLite next to `vault.db` (seq, author, label, time, ops, inverse ops). A History panel lists commits; **Roll back** commits the inverse ops as a new batch (history is never rewritten). Snapshots every N commits keep join fast.
- Session persistence across server restarts and host hand-over for LAN sessions.

## 10. Open questions

1. Should a 2D / 3D claim's session be keyed by the project GUID (stable across renames) or by the Vault item id once claims themselves live in the Vault?
2. Locks: selection-based is simple but someone who leaves a selection on blocks others. Lease of 60 s idle, or "steal lock" for owners (stage 2)?
3. Does the session server write the authoritative scene.json to the Vault (a "claim checkpoint" item), or do clients only autosave locally?
4. Default team port: HTTP port + 1 (8741/UDP), or a fixed one so firewalls are simpler?
5. Viewers on read-only keys: allowed to hold selection presence (outline) but no locks?
