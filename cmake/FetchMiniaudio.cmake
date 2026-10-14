# miniaudio (single header, MIT-0 / public domain): playback device and
# wav / mp3 / flac decoding for ts_audio, plus stb_vorbis (public domain) for
# ogg. Both pinned and checked by hash; nothing is committed. The
# implementations are compiled once in src/audio/MiniaudioImpl.cpp.
include(FetchContent)
FetchContent_Declare(
  ts_miniaudio
  URL      https://raw.githubusercontent.com/mackron/miniaudio/0.11.21/miniaudio.h
  URL_HASH SHA256=6b2029714f8634c4d7c70cc042f45074e0565766113fc064f20cd27c986be9c9
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Declare(
  ts_stb_vorbis
  URL      https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c
  URL_HASH SHA256=4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(ts_miniaudio ts_stb_vorbis)

add_library(ts_miniaudio INTERFACE)
target_include_directories(ts_miniaudio INTERFACE
  ${ts_miniaudio_SOURCE_DIR} ${ts_stb_vorbis_SOURCE_DIR})
