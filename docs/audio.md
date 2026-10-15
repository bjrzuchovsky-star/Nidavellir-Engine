# Audio (`ts_audio`)

Source: `src/audio/` (`Engine.cpp`, `MiniaudioImpl.cpp`, `SoundStage*.cpp`, `Synth.cpp`, `MixerSettings.*`), `src/editor/audio/`.

## Backend

Audio runs on miniaudio 0.11.21 (wav, mp3, flac) plus stb_vorbis (ogg). With no sound card, or with `TS_AUDIO=null`, the engine uses a null backend that still mixes.

## Sounds

Put audio files in `<project>/assets`. Built-in placeholder sounds are synthesized in `Synth.cpp`:

`builtin:click`, `step`, `gunshot`, `mine`, `boom`, `grab`, `throw`, `chime`, `tune`

## Mixing and space

- **Buses:** master, music, sfx, ui and voice, each with volume and mute.
- **Music:** two decks with crossfade and playlists.
- **2D sound:** equal-power pan plus distance fade (`SoundStage`).
- **3D sound:** positional audio with the listener on the camera, or on the headset in VR. Linear attenuation and optional doppler (`SoundStage3D`).

## Audio Source component

Fields: clip, bus, volume, pitch, autoplay, loop, positional, near / far, doppler. It needs scene v8 in 2D and v10 in 3D.

## Lua

```lua
audio.play("builtin:chime", {volume=0.8, pitch=1.0, variation=0.1, bus="sfx", at=self})
audio.music("theme.ogg", 2.0); audio.playlist({"a.ogg","b.ogg"}, 1.5)
audio.stop_music(1.0); audio.set_volume("music", 0.5); audio.stop()
```

These functions are registered in `src/runtime/ScriptApi.cpp`.

## Editor and settings

- **Mixer:** *View > Saloon Band (Mixer)* (`MixerPanel.cpp`).
- **Previews:** the Supply Wagon plays sound previews.
- **Inspector:** edits Audio Sources in both 2D and 3D.
- **Saved levels:** mixer levels are saved to `TombStoneConfig/audio.json`, which `ts_game` and `ts_client` also read.

## Multiplayer

The server turns every sound into a `ts.audio` event, and the clients replay it.

Smoke test: `ts_admin --smoke-audio`.
