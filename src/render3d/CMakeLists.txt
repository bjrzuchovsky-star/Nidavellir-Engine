# ts_render3d: 3D math, meshes + primitives, glTF 2.0 loading (cgltf),
# images (stb_image via ts_gfx), the 3D scene format (scene.json v7) and
# two renderers behind one interface: null (headless) and an OpenGL 3.3
# core forward renderer (GlRenderer3D). Built with the windowed apps.
add_library(ts_render3d
  Json.cpp
  Mesh.cpp
  Gltf.cpp
  Scene3D.cpp
  SceneView.cpp
  Renderer3D.cpp
  GlApi.cpp
  GlRenderer3D.cpp
)
target_include_directories(ts_render3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_render3d
  PUBLIC
    ts_core
    ts_scene
    glfw
    OpenGL::GL
  PRIVATE
    ts_cgltf
    ts_stb_image
    ts_gfx
)
target_compile_features(ts_render3d PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_render3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
                                                 _CRT_SECURE_NO_WARNINGS)
endif()
