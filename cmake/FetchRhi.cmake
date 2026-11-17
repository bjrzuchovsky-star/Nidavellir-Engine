# Graphics backends: Vulkan headers + volk, and the shader cross-compile
# tools (glslang, SPIRV-Cross, DXC). Nothing here is committed; FetchContent
# downloads it into the build tree.

include(FetchContent)

set(TS_VULKAN_HEADERS_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "Vulkan-Headers git tag")
set(TS_VOLK_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "volk git tag (sources; the CMake of volk is not used)")
set(TS_SPIRV_CROSS_TAG "57e4efcc84112abcd426dc5ec248b5339bee04f2" CACHE STRING "SPIRV-Cross commit")

FetchContent_Declare(
  vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        ${TS_VULKAN_HEADERS_TAG}
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  volk_src
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG        ${TS_VOLK_TAG}
  GIT_SHALLOW    TRUE
)
set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  spirv_cross
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
  GIT_TAG        ${TS_SPIRV_CROSS_TAG}
)

if(WIN32)
  set(TS_GLSLANG_URL "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip")
  set(TS_GLSLANG_HASH "SHA256=82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b")
  set(TS_DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip")
  set(TS_DXC_HASH "SHA256=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")
else()
  set(TS_GLSLANG_URL "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-linux-x86_64-release.zip")
  set(TS_GLSLANG_HASH "SHA256=a3fc4f083b1793eb53e55fa3577ac9649ffbe0340715e30d116290fb5382393f")
  set(TS_DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz")
  set(TS_DXC_HASH "SHA256=96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1")
endif()

FetchContent_Declare(glslang_bin URL ${TS_GLSLANG_URL} URL_HASH ${TS_GLSLANG_HASH})
FetchContent_Declare(dxc_bin URL ${TS_DXC_URL} URL_HASH ${TS_DXC_HASH})

FetchContent_MakeAvailable(vulkan_headers volk_src spirv_cross glslang_bin dxc_bin)

if(NOT TARGET ts_volk)
  add_library(ts_volk STATIC ${volk_src_SOURCE_DIR}/volk.c)
  target_include_directories(ts_volk PUBLIC ${volk_src_SOURCE_DIR} ${vulkan_headers_SOURCE_DIR}/include)
  target_compile_definitions(ts_volk PUBLIC VK_NO_PROTOTYPES)
  if(WIN32)
    target_compile_definitions(ts_volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
  else()
    target_compile_definitions(ts_volk PUBLIC VK_USE_PLATFORM_XLIB_KHR)
  endif()
  if(MSVC)
    target_compile_definitions(ts_volk PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endif()

file(GLOB_RECURSE TS_GLSLANG_HITS
  "${glslang_bin_SOURCE_DIR}/bin/glslangValidator${CMAKE_EXECUTABLE_SUFFIX}"
  "${glslang_bin_SOURCE_DIR}/bin/glslang${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT TS_GLSLANG_HITS)
  file(GLOB_RECURSE TS_GLSLANG_HITS "${glslang_bin_SOURCE_DIR}/*glslang*")
endif()
list(GET TS_GLSLANG_HITS 0 TS_GLSLANG)
if(NOT EXISTS "${TS_GLSLANG}")
  message(FATAL_ERROR "glslang was fetched but glslangValidator was not found under ${glslang_bin_SOURCE_DIR}")
endif()

# DXC runs on the machine that builds, so the binary has to match
# CMAKE_HOST_SYSTEM_PROCESSOR. The Windows package has bin/arm64, bin/x64
# and bin/x86; a recursive glob picks arm64 first and then fails with
# WinError 216 (the executable is for another CPU).
set(NID_DXC_EXECUTABLE "" CACHE FILEPATH
  "Host dxc (or dxc.exe). Empty uses the fetched package for this CPU, then VULKAN_SDK.")

function(nid_host_dxc_arch out_var)
  string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _proc)
  if(_proc MATCHES "^(amd64|x86_64|x64)$")
    set(${out_var} "x64" PARENT_SCOPE)
  elseif(_proc MATCHES "^(arm64|aarch64)$")
    set(${out_var} "arm64" PARENT_SCOPE)
  elseif(_proc MATCHES "^(x86|i686|i386|win32)$")
    set(${out_var} "x86" PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

nid_host_dxc_arch(TS_DXC_HOST_ARCH)

# Reproducible default is the fetched host-arch compiler (bin/x64 on AMD64,
# which ships dxc.exe, dxcompiler.dll and dxil.dll together). VULKAN_SDK is
# only a fallback: SDK 1.4.x Bin has dxc.exe and dxcompiler.dll but no
# dxil.dll, because DXC 1.8+ signs DXIL itself.
set(_dxc_candidates "")
set(_dxc_required "")
if(NID_DXC_EXECUTABLE)
  list(APPEND _dxc_candidates "${NID_DXC_EXECUTABLE}")
  set(_dxc_required "${NID_DXC_EXECUTABLE}")
endif()
if(TS_DXC_HOST_ARCH)
  list(APPEND _dxc_candidates
    "${dxc_bin_SOURCE_DIR}/bin/${TS_DXC_HOST_ARCH}/dxc${CMAKE_EXECUTABLE_SUFFIX}")
endif()
# Linux release is a single bin/dxc with libraries in ../lib.
list(APPEND _dxc_candidates
  "${dxc_bin_SOURCE_DIR}/bin/dxc${CMAKE_EXECUTABLE_SUFFIX}"
  "${dxc_bin_SOURCE_DIR}/dxc${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT "$ENV{VULKAN_SDK}" STREQUAL "")
  list(APPEND _dxc_candidates
    "$ENV{VULKAN_SDK}/Bin/dxc.exe"
    "$ENV{VULKAN_SDK}/bin/dxc.exe"
    "$ENV{VULKAN_SDK}/Bin/dxc"
    "$ENV{VULKAN_SDK}/bin/dxc")
endif()

# Returns whether major.minor in `text` is at least 1.8.
function(nid_dxc_at_least_1_8 out_ok out_mm text)
  string(REGEX MATCH "([0-9]+)\\.([0-9]+)" _m "${text}")
  if(NOT CMAKE_MATCH_1)
    set(${out_ok} FALSE PARENT_SCOPE)
    set(${out_mm} "" PARENT_SCOPE)
    return()
  endif()
  set(_ok FALSE)
  if(CMAKE_MATCH_1 GREATER 1 OR (CMAKE_MATCH_1 EQUAL 1 AND CMAKE_MATCH_2 GREATER_EQUAL 8))
    set(_ok TRUE)
  endif()
  set(${out_ok} ${_ok} PARENT_SCOPE)
  set(${out_mm} "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

set(_dxc_found "")
set(_dxc_ver "")
set(_dxc_notes "")
foreach(_cand IN LISTS _dxc_candidates)
  if(NOT EXISTS "${_cand}")
    string(APPEND _dxc_notes "  ${_cand}: not found\n")
    if("${_cand}" STREQUAL "${_dxc_required}" AND NOT "${_dxc_required}" STREQUAL "")
      message(FATAL_ERROR
        "NID_DXC_EXECUTABLE='${_cand}' does not exist.\n"
        "Point it at the host dxc.exe, or leave it empty to use the fetched bin/${TS_DXC_HOST_ARCH} compiler.")
    endif()
    continue()
  endif()
  set(_wrong_arch FALSE)
  if(TS_DXC_HOST_ARCH STREQUAL "x64" AND _cand MATCHES "[/\\\\](arm64|x86)[/\\\\]")
    set(_wrong_arch TRUE)
  elseif(TS_DXC_HOST_ARCH STREQUAL "arm64" AND _cand MATCHES "[/\\\\](x64|x86)[/\\\\]")
    set(_wrong_arch TRUE)
  elseif(TS_DXC_HOST_ARCH STREQUAL "x86" AND _cand MATCHES "[/\\\\](x64|arm64)[/\\\\]")
    set(_wrong_arch TRUE)
  endif()
  if(_wrong_arch)
    string(APPEND _dxc_notes "  ${_cand}: wrong CPU for host ${CMAKE_HOST_SYSTEM_PROCESSOR}\n")
    if("${_cand}" STREQUAL "${_dxc_required}" AND NOT "${_dxc_required}" STREQUAL "")
      message(FATAL_ERROR
        "NID_DXC_EXECUTABLE='${_cand}' is for another CPU. This host is ${CMAKE_HOST_SYSTEM_PROCESSOR} "
        "(want bin/${TS_DXC_HOST_ARCH}). WinError 216 is that mismatch.")
    endif()
    continue()
  endif()

  get_filename_component(_cand_dir "${_cand}" DIRECTORY)
  set(_has_dxil TRUE)
  if(WIN32)
    if(NOT EXISTS "${_cand_dir}/dxcompiler.dll")
      string(APPEND _dxc_notes "  ${_cand}: no dxcompiler.dll beside it\n")
      if("${_cand}" STREQUAL "${_dxc_required}" AND NOT "${_dxc_required}" STREQUAL "")
        message(FATAL_ERROR "NID_DXC_EXECUTABLE='${_cand}' has no dxcompiler.dll beside it.")
      endif()
      continue()
    endif()
    if(NOT EXISTS "${_cand_dir}/dxil.dll")
      set(_has_dxil FALSE)
    endif()
  endif()

  execute_process(
    COMMAND "${_cand}" --version
    RESULT_VARIABLE _dxc_rc
    OUTPUT_VARIABLE _dxc_out
    ERROR_VARIABLE _dxc_err
  )
  if(NOT _dxc_rc EQUAL 0)
    string(APPEND _dxc_notes
      "  ${_cand}: dxc --version failed (exit ${_dxc_rc}). WinError 216 means the binary is for another CPU.\n")
    if("${_cand}" STREQUAL "${_dxc_required}" AND NOT "${_dxc_required}" STREQUAL "")
      message(FATAL_ERROR
        "NID_DXC_EXECUTABLE='${_cand}' did not run (exit ${_dxc_rc}) on ${CMAKE_HOST_SYSTEM_PROCESSOR}.\n"
        "${_dxc_out}${_dxc_err}")
    endif()
    continue()
  endif()
  string(STRIP "${_dxc_out}${_dxc_err}" _this_ver)
  if(WIN32 AND NOT _has_dxil)
    nid_dxc_at_least_1_8(_new_enough _mm "${_this_ver}")
    if(NOT _new_enough)
      string(APPEND _dxc_notes
        "  ${_cand}: no dxil.dll, and DXC '${_this_ver}' is older than 1.8 (the internal validator).\n")
      if("${_cand}" STREQUAL "${_dxc_required}" AND NOT "${_dxc_required}" STREQUAL "")
        message(FATAL_ERROR
          "NID_DXC_EXECUTABLE='${_cand}' has no dxil.dll and is older than DXC 1.8.\n${_this_ver}")
      endif()
      continue()
    endif()
    message(STATUS "DXC ${_cand} has no dxil.dll; DXC ${_mm} signs DXIL itself")
  endif()
  set(_dxc_found "${_cand}")
  set(_dxc_ver "${_this_ver}")
  break()
endforeach()

if(NOT _dxc_found)
  message(FATAL_ERROR
    "DXC was not found for host ${CMAKE_HOST_SYSTEM_PROCESSOR} (want bin/${TS_DXC_HOST_ARCH}).\n"
    "A missing dxil.dll is normal for the Vulkan SDK (DXC 1.8+). "
    "The fetched bin/${TS_DXC_HOST_ARCH} package is preferred and includes dxil.dll.\n"
    "${_dxc_notes}")
endif()

get_filename_component(TS_DXC_DIR "${_dxc_found}" DIRECTORY)
set(TS_DXC "${_dxc_found}" CACHE FILEPATH "dxc selected for this host" FORCE)
set(TS_DXC_DIR "${TS_DXC_DIR}" CACHE PATH "Directory of the selected dxc" FORCE)
message(STATUS "DXC (${CMAKE_HOST_SYSTEM_PROCESSOR}): ${TS_DXC}")
message(STATUS "DXC version: ${_dxc_ver}")

# Copy only the DLLs that sit beside this dxc. dxil.dll is optional:
# Vulkan SDK 1.4 Bin ships dxc.exe + dxcompiler.dll and no dxil.dll.
set(_dxc_runtime "")
if(WIN32)
  foreach(_dll dxcompiler.dll dxil.dll)
    if(EXISTS "${TS_DXC_DIR}/${_dll}")
      list(APPEND _dxc_runtime "${TS_DXC_DIR}/${_dll}")
    endif()
  endforeach()
endif()
set(TS_DXC_RUNTIME_DLLS "${_dxc_runtime}" CACHE INTERNAL
  "dxcompiler.dll and, when shipped, dxil.dll next to the selected dxc")

# Copy dxcompiler.dll and dxil.dll next to an executable that may compile
# HLSL at runtime. No-op when this host has no Windows DXC DLLs.
function(nidavellir_copy_dxc_runtime target)
  if(NOT TS_DXC_RUNTIME_DLLS)
    return()
  endif()
  foreach(_dll IN LISTS TS_DXC_RUNTIME_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_dll}" "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Copying ${_dll} next to ${target}")
  endforeach()
endfunction()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TS_SHADER_BLOBS "${CMAKE_BINARY_DIR}/generated/ShaderBlobs.inc")
set(TS_SHADER_SRC "${CMAKE_SOURCE_DIR}/shaders/rhi")
file(GLOB TS_SHADER_INPUTS "${TS_SHADER_SRC}/*.glsl" "${TS_SHADER_SRC}/*.vert" "${TS_SHADER_SRC}/*.frag")
add_custom_command(
  OUTPUT "${TS_SHADER_BLOBS}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/shader_bake/bake.py"
          --src "${TS_SHADER_SRC}"
          --out "${TS_SHADER_BLOBS}"
          --glslang "${TS_GLSLANG}"
          --spirv-cross "$<TARGET_FILE:spirv-cross>"
          --dxc "${TS_DXC}"
  DEPENDS ${TS_SHADER_INPUTS} "${CMAKE_SOURCE_DIR}/tools/shader_bake/bake.py" spirv-cross
  COMMENT "Cross-compiling shaders (glslang + SPIRV-Cross + DXC)"
  VERBATIM
)
add_custom_target(ts_shader_bake DEPENDS "${TS_SHADER_BLOBS}")

# Dear ImGui backends for the non-GL APIs. Compiled only for the editor.
if(TS_BUILD_ADMIN AND NOT TARGET ts_imgui_vk)
  add_library(ts_imgui_vk STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
  target_compile_definitions(ts_imgui_vk PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
  target_include_directories(ts_imgui_vk PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${vulkan_headers_SOURCE_DIR}/include)
  target_link_libraries(ts_imgui_vk PUBLIC ts_imgui ts_volk)
  if(WIN32)
    add_library(ts_imgui_dx12 STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    target_include_directories(ts_imgui_dx12 PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(ts_imgui_dx12 PUBLIC ts_imgui d3d12 dxgi)
    target_compile_definitions(ts_imgui_dx12 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
endif()
