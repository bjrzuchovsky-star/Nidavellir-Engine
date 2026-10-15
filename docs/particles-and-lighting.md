# Particles and lighting (`ts_fx`)

Source: `src/fx/Particles.*`, `src/fx/Light2D.*`, `src/runtime/World_Fx.cpp`, `src/editor/fx/FxInspector.*`, `src/render3d/GlRenderer3D.cpp`.

## Particles

Any 2D or 3D entity can carry a `particles` component (scene v9 for 2D, v11 for 3D). Emitter settings:

- **Emission:** burst and continuous.
- **Shapes:** point, circle / sphere, box, line.
- **Motion:** lifetime, speed, spread, gravity, drag, spin.
- **Curves:** 3-key size and colour curves.
- **Look:** a texture or sprite sheet, with alpha or additive blending.
- **Space:** local or world.
- **Seed:** deterministic. A seed of 0 derives it from the entity id.

Presets: `dust`, `gun_smoke`, `muzzle_flash`, `sparks`, `campfire`, `rain`, `sand_storm`, `thruster`, `explosion`, `sparkle`.

```lua
particles.burst("sparks", self, 20)
particles.burst("dust", {x = 100, y = 40})
particles.burst(self)
particles.emit(self, 10)
particles.stop(self); particles.start(self)
```

- **Editor:** the Particles inspector has a live preview in both 2D and 3D.
- **Multiplayer:** bursts are sent from the server as `ts.fx` events.
- **3D drawing:** particles render as camera-facing billboards.

## 2D lighting

Turn it on with the scene's `lighting` block (*View > Lighting* in the 2D editor). It sets:

- ambient tint
- an optional day / night cycle
- lightmap resolution

Entities can carry a `light` component (point or spot) with colour, intensity, radius, angle, cone, softness, flicker and shadows.

Solid tiles cast soft shadows using a CPU ray march.

Plugins can add lighting through the Registrar with `add_light_source`, `add_light_occluder`, `add_ambient` and `add_hud_layer`.

## 3D lighting

The 3D renderer supports:

- one directional sun with a 2048² shadow map and 3×3 PCF
- up to 8 point lights per frame
- hemisphere ambient
- a gradient sky
- Reinhard tone mapping

Set these up in the 3D editor's Environment panel.

Smoke test: `ts_admin --smoke-fx`.
