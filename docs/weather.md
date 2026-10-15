# Weather (`ts.weather`)

The optional **Weather** plugin (Trading Post, built-in, off by default) adds rain, snow, dust storms, fog and thunderstorms with lightning. It also adds gusting wind that pushes foliage and particles, and an optional time-of-day clock that moves the sky. It works in 2D, 3D and VR projects. Sources:

- `src/weather/Weather.*`: the deterministic simulation and `weather.json`;
- `src/weather/WeatherLua.*`;
- `src/weather/Weather3D.*`: the 3D World extension;
- `src/plugins/weather/WeatherPlugin.*`: 2D drawing and lighting;
- `src/editor/plugins/WeatherPanels.cpp`.

## Turning it on

Enable **Weather** for the project in the Trading Post (or in `project.json` plugins). The editor then gets **View > Weather**, and Play and exported games run the weather.

## weather.json

The file sits at the project root. If it is missing, defaults are used.

| Key | Default | Meaning |
|---|---|---|
| `start` | `"clear"` | `clear`, `rain`, `snow`, `dust`, `fog` or `storm` (an unknown kind falls back to clear) |
| `intensity` | 1 | 0 to 2; scales every effect |
| `transition` | 8 | Seconds to blend between kinds |
| `cycle`, `steps` | off | Loop through `[{"kind": "rain", "seconds": 60}, ...]` |
| `wind_dir`, `wind_scale`, `gust` | 30, 1, 0.35 | Degrees from +x towards +z (3D) or +y (2D); multiplier; gustiness |
| `lightning_min`, `lightning_max` | 4, 11 | Seconds between strikes in a full storm |
| `thunder` | true | Delayed low `builtin:boom` after a strike (3D) |
| `time_link`, `day_length`, `start_time` | off, 600, 0.35 | Run the clock (0 midnight, 0.5 noon); the sun and sky follow the hour |
| `seed` | 1 | Lightning timing and gusts are deterministic per seed |

The **Weather** panel edits these settings and saves the file. In edit mode, it previews the mix with kind buttons, Strike! and a time slider. During Play, the same controls drive the live weather.

## 3D and VR

Hosts (game, editor Play, VR) attach `weather::Weather3D` when the plugin is enabled. Each tick it:

- sets the environment: fog density and colour, overcast sky and dimmer sun, dust haze, lightning flash;
- with the time link on, moves the sun and sky with the hour;
- sets the wind, which bends foliage and pushes particle emitters;
- draws rain streaks, snowflakes and dust as billboards around the camera.

## 2D

The plugin draws rain streaks, snow, dust and fog banks as world quads in the `weather.air` layer, and a haze and flash tint in the HUD (`weather.tint`). It darkens the 2D lighting ambient under cloud and flashes it on a strike. A lightning light source (`weather.lightning`) casts shadows, and the wind pushes every particle emitter.

## Lua

```lua
weather.set("storm", 5)            -- kind, optional fade seconds
local now, target, blend = weather.get()
local wx, wz, strength = weather.wind()
weather.set_wind(90, 0.8)          -- degrees, strength (< 0 = back to the weather's own)
weather.set_time(0.75)             -- time of day 0..1
local t = weather.time()
weather.lightning()                -- strike now
weather.intensity(1.5)
local r = weather.amount("rain")   -- rain / snow / dust / fog / storm / cloud / flash / daylight

function on_weather(kind, storm) end  -- the target kind changed
function on_lightning(_, count) end   -- a strike (count so far)
```

In multiplayer, the server relays a script's `weather.set` to clients on the `weather` net channel.

## Smoke

`ts_admin --smoke-weather` covers:

- the simulation: determinism, a storm with strikes, blending, the cycle, time of day;
- the `weather.json` round trip;
- the 2D quads and ambient, and particle wind;
- the 3D fog environment, the Lua `weather` table, snow billboards and `on_lightning`.
