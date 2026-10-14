#pragma once

// Sound (ts_audio) on the null backend: decoding (wav / ogg / mp3 / flac),
// built-in sounds, bus mixing levels, pan / attenuation / doppler math,
// music crossfade + playlists, pitch and variation; the audio source in
// scene.json (2D v8, 3D v10), sound events (wire format, server -> client,
// Lua audio.*) and the SoundStage hosts drive.
int run_audio_smoke();
