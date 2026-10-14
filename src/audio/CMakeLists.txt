# Sound (ts_audio): buses, voices, music decks with crossfade, spatial math,
# built-in synthesized placeholder sounds. miniaudio opens the playback
# device and decodes wav / mp3 / flac, stb_vorbis decodes ogg (both fetched
# by hash). Without a device it runs on the null backend, so headless smokes
# and servers can link it. SoundStage plays a runtime World's sound events
# and audio-source components (hosts drive it).
add_library(ts_audio
  Engine.cpp
  MiniaudioImpl.cpp
  Synth.cpp
  SoundStage.cpp
)

target_include_directories(ts_audio
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_audio
  PUBLIC
    ts_core
    ts_runtime
  PRIVATE
    ts_miniaudio
    ${CMAKE_DL_LIBS}
)

find_package(Threads REQUIRED)
target_link_libraries(ts_audio PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(ts_audio PRIVATE m)
endif()

# Third-party single-file libraries: their warnings are not ours (MSVC:
# #pragma warning(push, 0) in the file; a /w flag would clash with /W1).
if(NOT MSVC)
  set_source_files_properties(MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_compile_features(ts_audio PUBLIC cxx_std_20)
