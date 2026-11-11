#include "gfx/TextureCache.h"

#include "core/MemImages.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace ts {
namespace tombstone {
namespace gfx {

namespace fs = std::filesystem;

namespace {
TextureUploader* g_uploader = nullptr;

// Asset-browser thumbs don't need the full plate. 512 on the longest edge
// is enough for a tile and keeps glTexImage2D off the multi-megabyte path.
void downscale_thumb(std::vector<unsigned char>* rgba, int* w, int* h, int max_edge) {
  if (!rgba || !w || !h || *w <= 0 || *h <= 0) return;
  if (*w <= max_edge && *h <= max_edge) return;
  const double s = std::min(static_cast<double>(max_edge) / *w, static_cast<double>(max_edge) / *h);
  const int nw = std::max(1, static_cast<int>(std::lround(*w * s)));
  const int nh = std::max(1, static_cast<int>(std::lround(*h * s)));
  std::vector<unsigned char> dst(static_cast<std::size_t>(nw) * static_cast<std::size_t>(nh) * 4u);
  const unsigned char* src = rgba->data();
  for (int y = 0; y < nh; ++y) {
    const int y0 = y * *h / nh;
    const int y1 = std::max(y0 + 1, (y + 1) * *h / nh);
    for (int x = 0; x < nw; ++x) {
      const int x0 = x * *w / nw;
      const int x1 = std::max(x0 + 1, (x + 1) * *w / nw);
      unsigned sum[4] = {};
      unsigned n = 0;
      for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
          const unsigned char* p = src + (static_cast<std::size_t>(yy) * static_cast<std::size_t>(*w) + xx) * 4u;
          for (int c = 0; c < 4; ++c) sum[c] += p[c];
          ++n;
        }
      }
      unsigned char* o = dst.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(nw) + x) * 4u;
      if (n == 0) n = 1;
      for (int c = 0; c < 4; ++c) o[c] = static_cast<unsigned char>(sum[c] / n);
    }
  }
  *rgba = std::move(dst);
  *w = nw;
  *h = nh;
}
}  // namespace

void set_texture_uploader(TextureUploader* uploader) { g_uploader = uploader; }
TextureUploader* texture_uploader() { return g_uploader; }

TextureCache::~TextureCache() { clear(); }

namespace {
ExtraImageDecoder g_extra_decoder = nullptr;
}  // namespace

void set_extra_image_decoder(ExtraImageDecoder decoder) { g_extra_decoder = decoder; }

bool TextureCache::decode_file(const std::string& path,
                               std::vector<unsigned char>* rgba, int* width,
                               int* height, std::string* error) {
  // Read through std::filesystem/ifstream (same path handling as the rest of
  // the engine), then decode from memory.
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "missing file";
    return false;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  if (bytes.empty() || bytes.size() > 0x7fffffffu) {
    if (error) *error = "empty or oversized file";
    return false;
  }
  int iw = 0, ih = 0, ic = 0;
  if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &ic)) {
    if (iw > 8192 || ih > 8192) {
      if (error) *error = "image is " + std::to_string(iw) + "x" + std::to_string(ih) + ", over the 8192 limit";
      return false;
    }
  }
  if (g_extra_decoder && g_extra_decoder(bytes.data(), bytes.size(), rgba, width, height)) return true;
  int w = 0;
  int h = 0;
  int comp = 0;
  unsigned char* px = stbi_load_from_memory(
      bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
  if (!px) {
    if (error) {
      const char* why = stbi_failure_reason();
      *error = std::string("decode failed: ") + (why ? why : "unknown");
    }
    return false;
  }
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    stbi_image_free(px);
    if (error) *error = "image is " + std::to_string(w) + "x" + std::to_string(h) + ", over the 8192 limit";
    return false;
  }
  if (rgba) {
    const auto n = static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) * 4ull;
    rgba->assign(px, px + static_cast<std::size_t>(n));
  }
  stbi_image_free(px);
  if (width) *width = w;
  if (height) *height = h;
  return true;
}

void TextureCache::release(Entry& entry) {
  if (entry.info.handle != 0 && entry.owner != nullptr &&
      entry.owner == g_uploader) {
    g_uploader->destroy(entry.info.handle);
  }
  entry.info.handle = 0;
  entry.owner = nullptr;
}

void TextureCache::load_into(const std::string& path, Entry& entry) {
  release(entry);
  entry.info = TextureInfo{};
  if (mem_images::is_key(path)) {
    const mem_images::MemImage* m = mem_images::get(path);
    entry.had_file = m != nullptr;
    entry.mem_version = m ? m->version : 0;
    if (!m || m->width <= 0 || m->height <= 0) {
      entry.info.error = "missing memory image";
      return;
    }
    entry.info.width = m->width;
    entry.info.height = m->height;
    entry.info.ok = true;
    if (g_uploader) {
      entry.info.handle = g_uploader->upload_rgba(m->rgba.data(), m->width, m->height);
      entry.owner = entry.info.handle != 0 ? g_uploader : nullptr;
    }
    return;
  }
  std::error_code ec;
  const fs::path p = fs::path(path);
  entry.had_file = fs::is_regular_file(p, ec);
  entry.mtime = entry.had_file ? fs::last_write_time(p, ec)
                               : fs::file_time_type{};
  if (!entry.had_file) {
    entry.info.error = "missing file";
    return;
  }
  std::vector<unsigned char> rgba;
  if (!decode_file(path, &rgba, &entry.info.width, &entry.info.height,
                   &entry.info.error)) {
    return;
  }
  entry.info.ok = true;
  // The tile draws a small square. Upload a preview, not the full 4K plate,
  // so opening a folder of textures doesn't stall the frame. Width and
  // height stay the file's, for the tile's aspect.
  int uw = entry.info.width, uh = entry.info.height;
  downscale_thumb(&rgba, &uw, &uh, 512);
  if (g_uploader) {
    entry.info.handle = g_uploader->upload_rgba(rgba.data(), uw, uh);
    entry.owner = entry.info.handle != 0 ? g_uploader : nullptr;
  }
}

const TextureInfo& TextureCache::get(const std::string& path) {
  auto it = entries_.find(path);
  if (it != entries_.end()) {
    if (mem_images::is_key(path)) {
      const mem_images::MemImage* m = mem_images::get(path);
      if ((m ? m->version : 0) != it->second.mem_version) {
        load_into(path, it->second);
      }
    }
    return it->second.info;
  }
  Entry& entry = entries_[path];
  load_into(path, entry);
  return entry.info;
}

std::size_t TextureCache::poll_changes(double now, double interval) {
  if (now - last_poll_ < interval) {
    return 0;
  }
  last_poll_ = now;
  std::size_t reloaded = 0;
  for (auto& [path, entry] : entries_) {
    if (mem_images::is_key(path)) continue;  // checked on every get()
    std::error_code ec;
    const fs::path p = fs::path(path);
    const bool has = fs::is_regular_file(p, ec);
    const fs::file_time_type mt =
        has ? fs::last_write_time(p, ec) : fs::file_time_type{};
    if (has != entry.had_file || (has && mt != entry.mtime)) {
      load_into(path, entry);
      ++reloaded;
    }
  }
  return reloaded;
}

void TextureCache::invalidate(const std::string& path) {
  auto it = entries_.find(path);
  if (it != entries_.end()) {
    release(it->second);
    entries_.erase(it);
  }
}

void TextureCache::clear() {
  for (auto& [path, entry] : entries_) {
    release(entry);
  }
  entries_.clear();
}

}  // namespace gfx
}  // namespace tombstone
}  // namespace ts
