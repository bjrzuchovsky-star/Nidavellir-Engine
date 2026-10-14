// The one translation unit compiling miniaudio and stb_vorbis (both fetched
// by hash, see cmake/FetchMiniaudio.cmake). Third-party warnings are muted
// for this file only (src/audio/CMakeLists.txt; MSVC via the pragma).

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "audio/Audio.h"
#include "audio/Backend.h"

#include <cstdlib>
#include <cstring>

namespace ts {
namespace audio {

namespace {

bool decode_with(ma_decoder* dec, Clip* out, std::string* error) {
  out->channels = static_cast<int>(dec->outputChannels);
  out->rate = static_cast<int>(dec->outputSampleRate);
  out->samples.clear();
  std::vector<float> chunk(4096 * static_cast<std::size_t>(out->channels));
  for (;;) {
    ma_uint64 got = 0;
    const ma_result r = ma_decoder_read_pcm_frames(dec, chunk.data(), 4096, &got);
    out->samples.insert(out->samples.end(), chunk.begin(),
                        chunk.begin() + static_cast<std::ptrdiff_t>(got * out->channels));
    if (r != MA_SUCCESS || got < 4096) break;
    if (out->samples.size() > 48000u * 2u * 600u) {  // 10 min cap
      if (error) *error = "clip too long (use it as music)";
      return false;
    }
  }
  if (out->samples.empty()) {
    if (error) *error = "no audio frames";
    return false;
  }
  return true;
}

ma_decoder_config config_for(int rate, int channels) {
  ma_decoder_config cfg =
      ma_decoder_config_init(ma_format_f32, static_cast<ma_uint32>(channels),
                             static_cast<ma_uint32>(rate));
  return cfg;
}

// Mono stays mono, everything else is folded to stereo.
int pick_channels(ma_decoder* probe) {
  return probe->outputChannels == 1 ? 1 : 2;
}

}  // namespace

bool decode_memory(const void* data, std::size_t size, int rate, Clip* out,
                   std::string* error) {
  if (!data || size == 0 || !out) {
    if (error) *error = "empty buffer";
    return false;
  }
  ma_decoder probe;
  ma_decoder_config pc = config_for(rate, 0);
  if (ma_decoder_init_memory(data, size, &pc, &probe) != MA_SUCCESS) {
    if (error) *error = "unrecognised audio data";
    return false;
  }
  const int ch = pick_channels(&probe);
  ma_decoder_uninit(&probe);
  ma_decoder dec;
  ma_decoder_config cfg = config_for(rate, ch);
  if (ma_decoder_init_memory(data, size, &cfg, &dec) != MA_SUCCESS) {
    if (error) *error = "decoder failed";
    return false;
  }
  const bool ok = decode_with(&dec, out, error);
  ma_decoder_uninit(&dec);
  return ok;
}

bool decode_file(const std::string& path, int rate, Clip* out,
                 std::string* error) {
  ma_decoder probe;
  ma_decoder_config pc = config_for(rate, 0);
  if (ma_decoder_init_file(path.c_str(), &pc, &probe) != MA_SUCCESS) {
    if (error) *error = "cannot open or decode " + path;
    return false;
  }
  const int ch = pick_channels(&probe);
  ma_decoder_uninit(&probe);
  ma_decoder dec;
  ma_decoder_config cfg = config_for(rate, ch);
  if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
    if (error) *error = "decoder failed for " + path;
    return false;
  }
  const bool ok = decode_with(&dec, out, error);
  ma_decoder_uninit(&dec);
  return ok;
}

namespace detail {

namespace {

class FileStream final : public Stream {
 public:
  ~FileStream() override {
    if (open_) ma_decoder_uninit(&dec_);
  }
  bool open(const std::string& path, int rate) {
    ma_decoder_config cfg = config_for(rate, 2);
    open_ = ma_decoder_init_file(path.c_str(), &cfg, &dec_) == MA_SUCCESS;
    return open_;
  }
  int read(float* stereo, int frames) override {
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&dec_, stereo, static_cast<ma_uint64>(frames), &got);
    return static_cast<int>(got);
  }
  bool rewind() override {
    return ma_decoder_seek_to_pcm_frame(&dec_, 0) == MA_SUCCESS;
  }

 private:
  ma_decoder dec_{};
  bool open_ = false;
};

class MaDevice final : public DeviceHandle {
 public:
  ~MaDevice() override {
    if (dev_open_) ma_device_uninit(&dev_);
    if (ctx_open_) ma_context_uninit(&ctx_);
  }
  bool open(int rate, MixCallback cb, void* user, std::string* error) {
    cb_ = cb;
    user_ = user;
    if (ma_context_init(nullptr, 0, nullptr, &ctx_) != MA_SUCCESS) {
      if (error) *error = "no audio context";
      return false;
    }
    ctx_open_ = true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(rate);
    cfg.dataCallback = &MaDevice::callback;
    cfg.pUserData = this;
    if (ma_device_init(&ctx_, &cfg, &dev_) != MA_SUCCESS) {
      if (error) *error = "no playback device";
      return false;
    }
    dev_open_ = true;
    if (ma_device_start(&dev_) != MA_SUCCESS) {
      if (error) *error = "playback device would not start";
      return false;
    }
    name_ = std::string(ma_get_backend_name(ctx_.backend));
    return true;
  }
  std::string name() const override { return name_; }

 private:
  static void callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    auto* self = static_cast<MaDevice*>(dev->pUserData);
    self->cb_(self->user_, static_cast<float*>(out), static_cast<int>(frames));
  }
  ma_context ctx_{};
  ma_device dev_{};
  bool ctx_open_ = false;
  bool dev_open_ = false;
  MixCallback cb_ = nullptr;
  void* user_ = nullptr;
  std::string name_;
};

}  // namespace

std::unique_ptr<Stream> open_stream(const std::string& path, int rate,
                                    std::string* error) {
  auto s = std::make_unique<FileStream>();
  if (!s->open(path, rate)) {
    if (error) *error = "cannot stream " + path;
    return nullptr;
  }
  return s;
}

std::unique_ptr<DeviceHandle> open_device(int rate, MixCallback callback,
                                          void* user, std::string* error) {
  auto d = std::make_unique<MaDevice>();
  if (!d->open(rate, callback, user, error)) return nullptr;
  return d;
}

}  // namespace detail
}  // namespace audio
}  // namespace ts
