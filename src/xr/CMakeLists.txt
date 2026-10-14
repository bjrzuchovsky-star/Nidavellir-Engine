# ts_xr: VR sessions for "dimension": "vr" projects. OpenXR (Khronos loader,
# fetched by hash; headset rendering through XR_KHR_opengl_enable on
# Windows) and the desktop simulator, plus the VrPresenter that drives a
# World3D in VR mode and renders both eyes with ts_render3d.
add_library(ts_xr
  XrSession.cpp
  OpenXrSession.cpp
  VrPresenter.cpp
)
target_include_directories(ts_xr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_xr PUBLIC ts_render3d ts_world3d)
target_compile_features(ts_xr PUBLIC cxx_std_20)
if(TS_HAVE_OPENXR)
  target_link_libraries(ts_xr PRIVATE openxr_loader)
  target_compile_definitions(ts_xr PRIVATE TS_HAVE_OPENXR=1)
endif()
if(MSVC)
  target_compile_definitions(ts_xr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
