#include "rhi/ShaderLib.h"

#include <cstring>

#include "ShaderBlobs.inc"

namespace ts {
namespace tombstone {
namespace rhi {

const ShaderCode* find_shader(const char* name) {
  if (!name) return nullptr;
  static ShaderCode table[48]{};
  static unsigned count = 0;
  if (count == 0) {
    count = kTsShaderCount;
    for (unsigned i = 0; i < count && i < 48; ++i) {
      table[i].name = kTsShaders[i].name;
      table[i].glsl = kTsShaders[i].glsl;
      table[i].spv = kTsShaders[i].spv;
      table[i].spv_words = kTsShaders[i].spv_words;
      table[i].dxil = kTsShaders[i].dxil;
      table[i].dxil_bytes = kTsShaders[i].dxil_bytes;
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    if (std::strcmp(table[i].name, name) == 0) return &table[i];
  }
  return nullptr;
}

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
