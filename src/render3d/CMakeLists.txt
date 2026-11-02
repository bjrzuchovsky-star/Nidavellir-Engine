# ts_render3d: 3D math, meshes + primitives, glTF 2.0 loading (cgltf),
# universal model import (ModelImport: Source .mdl, Assimp formats, .blend),
# images (stb_image via ts_gfx), the 3D scene format (scene.json v7) and
# two renderers behind one interface: null (headless) and an OpenGL 3.3
# core forward renderer (GlRenderer3D). Built with the windowed apps.
add_library(ts_render3d
  Mesh.cpp
  Gltf.cpp
  GltfSkinned.cpp
  ModelImport.cpp
  ColliderFit.cpp
  HullFit.cpp
  MeshSplit.cpp
  import/SourceMdl.cpp
  import/SourceVtf.cpp
  import/SourceMaterials.cpp
  import/Gma.cpp
  import/Lzma.cpp
  import/AssimpImport.cpp
  import/BlenderImport.cpp
  Skeleton.cpp
  ClipFile.cpp
  Animator.cpp
  Scene3D.cpp
  Attach3D.cpp
  CameraMigrate3D.cpp
  Scene3D_Physics.cpp
  SceneView.cpp
  Renderer3D.cpp
  GlApi.cpp
  GlRenderer3D.cpp
  GlTerrainFoliage.cpp
  ../terrain/Terrain.cpp
  ../terrain/Foliage.cpp
  ../terrain/TerrainView.cpp
  HiddenGl.cpp
)
target_include_directories(ts_render3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_render3d
  PUBLIC
    ts_core
    ts_scene
    ts_profiler
    glfw
    OpenGL::GL
  PRIVATE
    ts_cgltf
    ts_stb_image
    ts_gfx
)
target_compile_features(ts_render3d PUBLIC cxx_std_20)
# Universal model import (ModelImport.h): Assimp importers when enabled.
if(TS_WITH_ASSIMP AND TARGET assimp)
  target_link_libraries(ts_render3d PRIVATE assimp)
  target_compile_definitions(ts_render3d PRIVATE TS_WITH_ASSIMP=1)
else()
  target_compile_definitions(ts_render3d PRIVATE TS_WITH_ASSIMP=0)
endif()
if(WIN32)
  # Blender lookup in the registry (RegGetValueW).
  target_link_libraries(ts_render3d PRIVATE advapi32)
endif()
if(MSVC)
  target_compile_definitions(ts_render3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
                                                 _CRT_SECURE_NO_WARNINGS)
endif()
