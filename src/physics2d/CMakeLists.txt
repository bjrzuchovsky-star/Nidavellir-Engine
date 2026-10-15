# ts_physics2d: 2D rigid bodies (Box2D v3 underneath, never in a public
# header). Used by the 2D World for rigid_body entities and by the
# Shipyard's optional Box2D hulls. Built for every app that runs the game.
add_library(ts_physics2d Physics2D.cpp)
target_include_directories(ts_physics2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_physics2d PRIVATE box2d)
target_compile_features(ts_physics2d PUBLIC cxx_std_20)
