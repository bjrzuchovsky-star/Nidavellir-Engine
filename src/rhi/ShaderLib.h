#pragma once

#include <cstdint>

namespace ts {
namespace tombstone {
namespace rhi {

struct ShaderCode {
  const char* name = nullptr;
  const char* glsl = nullptr;
  const unsigned int* spv = nullptr;
  unsigned spv_words = 0;
  const unsigned char* dxil = nullptr;
  unsigned dxil_bytes = 0;
};

const ShaderCode* find_shader(const char* name);

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
