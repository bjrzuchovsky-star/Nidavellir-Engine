#pragma once

#include "rhi/Rhi.h"

namespace ts {
namespace tombstone {
namespace rhi {

TextureRegistrar texture_registrar();
TextureUnregistrar texture_unregistrar();
void* texture_registrar_user();
void set_uniform_align(std::size_t align);

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
