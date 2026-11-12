// Single translation unit holding the stb_image implementation.
// stb_image (https://github.com/nothings/stb) is public domain / MIT
// (dual-licensed, see the header's license block). It is fetched at configure
// time by cmake/FetchImGuiDeps.cmake (admin and game builds).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO  // we decode from memory (see TextureCache.cpp)
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_FAILURE_USERMSG
// A corrupt header must not allocate a multi-gigabyte bitmap. 8192 is the
// same hard cap as render3d::kHardMaxTextureEdge.
#define STBI_MAX_DIMENSIONS 8192
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
