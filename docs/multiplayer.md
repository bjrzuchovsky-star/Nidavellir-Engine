# Multiplayer (the posse)

Source: `src/net/` (Transport, `EnetTransport.cpp`, `SimTransport.*`, `ByteStream.*`), `src/mp/` (`Protocol.*`, `GameServer.*`, `GameClient.*`, `LocalPosse.*`, `PosseSmoke.*`), `apps/server`, `apps/client`, `src/editor/screens/Editor2DScreen_Posse.cpp`.

Multiplayer is server-authoritative with up to 4 riders. It runs over ENet on UDP. The default port is **24642** (`kDefaultPort` in `src/net/Net.h`).

## Running a server and clients

```bash
ts_server --project <dir> [--port N] [--snapshot-every 2] [--lag MS --jitter MS --loss PCT] [--seconds S]
ts_client --connect host[:port] --name Ike [--project <dir>] [--interp 100] [--lag/--jitter/--loss] [--frames N]
ts_server --smoke [--riders N ...]   ;  ts_client --smoke
```

`ts_server` is headless (no GLFW, no GL) and loads the project's plugins. On Windows, ENet links `ws2_32` and `winmm`. Open the UDP port in the firewall for riders on other machines.

## How it works

- **Handshake:** the client sends Hello (protocol version, name, scene hash). The server answers with Welcome (slot, tick, snapshot rate and the full `scene.json`) or Reject. A fifth rider is told "The posse is full".
- **Input:** clients send input at 60 Hz, unreliably, repeating the last 8 inputs. The server keeps a jitter buffer per rider and applies each input exactly once.
- **Snapshots:** sent unreliably every `snapshot_every` ticks (30 Hz by default). They carry the last applied input, an RTT echo, and entity rows, including script spawns and destroys.
- **Events:** sent reliably. They include joins and leaves, toasts, Telegraph lines, the roster, `ts.audio` / `ts.fx` events and the `Plugin` message (protocol v2).
- **Prediction:** the client predicts its own rider with `World::predict_rider`, then reconciles against snapshots. Remote riders are interpolated `--interp` ms in the past.
- **Scripts:** run only on the server.
- **Plugins:** can add net channels through `World::net_post` / `net_deliver`.

## Play with Posse (editor)

Use the toolbar's **Posse** button or *Play > Play with Posse*. It starts a `GameServer` and 1-4 `GameClient`s inside the editor (`LocalPosse`). The **Network** panel has:

- **Posse:** ride, Break Camp, Saddle Another, Send One Home, Open Client Window
- **Rough Trail:** lag, jitter and loss with the presets Town, Prairie and Mountain Pass
- **Server and Riders:** stats, plus **Look** to view through another rider
- **Trail Map**

Smoke test: `run_posse_smoke`, which `ts_server --smoke`, `ts_client --smoke` and `ts_admin --smoke` all use.
