#pragma once

// ts_audio internals: the miniaudio side (streams, decoding, the playback
// device), kept behind this small interface so only MiniaudioImpl.cpp sees
// the 90k-line header.

#include <memory>
#include <string>

namespace ts {
namespace audio {
namespace detail {

// A file decoded on the fly to stereo float at `rate` (music decks).
class Stream {
 public:
  virtual ~Stream() = default;
  // Frames actually read (< frames at the end).
  virtual int read(float* stereo, int frames) = 0;
  virtual bool rewind() = 0;
};
std::unique_ptr<Stream> open_stream(const std::string& path, int rate,
                                    std::string* error);

// Playback device calling `callback(user, out, frames)` (stereo float) from
// the audio thread. Null when no device opens.
using MixCallback = void (*)(void* user, float* out, int frames);
class DeviceHandle {
 public:
  virtual ~DeviceHandle() = default;
  virtual std::string name() const = 0;
};
std::unique_ptr<DeviceHandle> open_device(int rate, MixCallback callback,
                                          void* user, std::string* error);

}  // namespace detail
}  // namespace audio
}  // namespace ts
