add_library(ts_rhi
  Rhi.cpp
  ShaderLib.cpp
  GlDevice.cpp
  VkDevice.cpp
  Dx12Device.cpp
  ../render3d/GlApi.cpp
)
add_dependencies(ts_rhi ts_shader_bake)
target_include_directories(ts_rhi
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
  PRIVATE ${CMAKE_BINARY_DIR}/generated ${vulkan_headers_SOURCE_DIR}/include)
target_link_libraries(ts_rhi
  PUBLIC glfw OpenGL::GL ts_volk
)
if(WIN32)
  target_link_libraries(ts_rhi PUBLIC d3d12 dxgi dxguid)
  target_compile_definitions(ts_rhi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_compile_features(ts_rhi PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_rhi PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
