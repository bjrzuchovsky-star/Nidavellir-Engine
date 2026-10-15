#pragma once

// ts_vfs: mount table over .tspak archives with loose-file fallback.
// Lookups try mounted paks (last mounted wins), then the loose roots.
//
// mount_project_pak() is the runtime's entry point: it verifies a pak and
// lays its files out once in a per-hash cache folder (reused while the hash
// matches), so every path-based loader (scene, scripts, UI, audio via
// miniaudio, textures, glTF, plugin data) reads the packed content with no
// per-loader change.

#include "vfs/Pak.h"

#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace vfs {

class Vfs {
 public:
  bool mount_pak(const std::string& pak_path, std::string* error);
  void add_loose_root(const std::string& dir);

  bool exists(const std::string& rel) const;
  bool read(const std::string& rel, std::string* out, std::string* error = nullptr) const;
  // Every known path under prefix ("" = all), paks and loose, sorted, unique.
  std::vector<std::string> list(const std::string& prefix = "") const;

  const std::vector<std::unique_ptr<PakReader>>& paks() const { return paks_; }

 private:
  std::vector<std::unique_ptr<PakReader>> paks_;
  std::vector<std::string> loose_;
};

struct MountResult {
  std::string dir;           // folder to hand to the loaders
  std::uint64_t hash = 0;    // pak content hash
  std::size_t files = 0;
  bool reused = false;       // cache already held this hash
};

// Default cache root: $TS_PAK_CACHE, else %LOCALAPPDATA%/TombStone/paks
// (Windows) / $XDG_CACHE_HOME or ~/.cache/tombstone/paks, else temp.
std::string default_cache_root();

bool mount_project_pak(const std::string& pak_path, const std::string& cache_root,
                       MountResult* out, std::string* error);

}  // namespace vfs
}  // namespace tombstone
}  // namespace ts
