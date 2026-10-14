# In-game UI (ts_ui): bundled fonts baked with stb_truetype, retained
# widget documents (<project>/ui/*.ui.json), layout, navigation, drawing
# and a software rasteriser. Pure C++ (no GL): hosts draw the quads.
set(TS_UI_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${TS_UI_GEN}")
ts_embed_binary("${TS_FONT_BODY_TTF}" "${TS_UI_GEN}/FontBody.cpp" ts_font_body_ttf)
ts_embed_binary("${TS_FONT_DISPLAY_TTF}" "${TS_UI_GEN}/FontDisplay.cpp" ts_font_display_ttf)

add_library(ts_ui
  Font.cpp
  UiData.cpp
  UiLayout.cpp
  UiSystem.cpp
  ${TS_UI_GEN}/FontBody.cpp
  ${TS_UI_GEN}/FontDisplay.cpp
)

target_include_directories(ts_ui
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_ui
  PUBLIC
    ts_core
  PRIVATE
    ts_scene
    ts_stb_truetype
)

target_compile_features(ts_ui PUBLIC cxx_std_20)
