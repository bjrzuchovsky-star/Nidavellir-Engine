#pragma once

// Dynamic plugins (.dll / .so / .dylib): a small C entry point the host
// finds with LoadLibrary / dlopen, so the handshake itself does not depend
// on C++ name mangling.
//
//   // my_plugin.cpp, built as a CMake MODULE library
//   #include "plugin/PluginAbi.h"
//   class MyPlugin : public ts::tombstone::plugin::Plugin { ... };
//   TS_DYNAMIC_PLUGIN(MyPlugin)
//
// The host checks abi_version, struct_size, api_tag and cxx_tag before it
// calls create(). The object create() returns is a C++ Plugin*, so a
// dynamic plugin must be built against the same Plugin.h (api_tag) with a
// compatible C++ runtime (cxx_tag: libstdc++ / libc++ / MSVC release vs
// debug CRT). "ABI-ish": safe for plugins built with the engine's own
// toolchain, refused cleanly otherwise. destroy() frees on the plugin's
// side of the boundary (its own heap on Windows).

#include "plugin/Plugin.h"

#include <cstdint>

#define TS_PLUGIN_ABI_VERSION 1u
// Bump when Plugin.h / WorldExtensions change in a way old binaries break.
#define TS_PLUGIN_API_TAG "tombstone-plugin-api/1"
#define TS_PLUGIN_ENTRY_NAME "ts_plugin_entry"

#if defined(_MSC_VER)
#if defined(_DEBUG)
#define TS_PLUGIN_CXX_TAG "msvc-debug-crt"
#else
#define TS_PLUGIN_CXX_TAG "msvc-release-crt"
#endif
#elif defined(_LIBCPP_VERSION)
#define TS_PLUGIN_CXX_TAG "libc++"
#else
#define TS_PLUGIN_CXX_TAG "libstdc++"
#endif

#if defined(_WIN32)
#define TS_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define TS_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern "C" {

typedef struct TsPluginEntry {
  std::uint32_t abi_version;  // TS_PLUGIN_ABI_VERSION
  std::uint32_t struct_size;  // sizeof(TsPluginEntry)
  const char* api_tag;        // TS_PLUGIN_API_TAG
  const char* cxx_tag;        // TS_PLUGIN_CXX_TAG
  void* (*create)(void);      // new Plugin (as void*)
  void (*destroy)(void*);     // delete what create() made
} TsPluginEntry;

typedef const TsPluginEntry* (*TsPluginEntryFn)(void);

}  // extern "C"

#define TS_DYNAMIC_PLUGIN(CLASS)                                          \
  namespace {                                                             \
  void* ts_plugin_create_() {                                             \
    try {                                                                 \
      return static_cast<::ts::tombstone::plugin::Plugin*>(new CLASS());  \
    } catch (...) {                                                       \
      return nullptr;                                                     \
    }                                                                     \
  }                                                                       \
  void ts_plugin_destroy_(void* p) {                                      \
    delete static_cast<::ts::tombstone::plugin::Plugin*>(p);              \
  }                                                                       \
  }                                                                       \
  TS_PLUGIN_EXPORT const TsPluginEntry* ts_plugin_entry(void) {           \
    static const TsPluginEntry entry = {                                  \
        TS_PLUGIN_ABI_VERSION,      sizeof(TsPluginEntry),                \
        TS_PLUGIN_API_TAG,          TS_PLUGIN_CXX_TAG,                    \
        &ts_plugin_create_,         &ts_plugin_destroy_};                 \
    return &entry;                                                        \
  }
