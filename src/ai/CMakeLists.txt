# AI toolkit (ts_ai): grid A* / JPS / HPA*, flow fields, steering, Recast
# navmesh + Detour crowd, behaviour trees + utility scoring, perception and
# a state machine helper. Pure C++ (no window, no GL): the headless server
# runs AI too. The ts.ai plugin (src/plugins/ai) wires it into worlds.
add_library(ts_ai
  AiJson.cpp
  GridPath.cpp
  Steering.cpp
  BehaviorTree.cpp
  Perception.cpp
  NavMesh.cpp
)

target_include_directories(ts_ai
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_ai PRIVATE ts_recast)
target_compile_features(ts_ai PUBLIC cxx_std_20)
