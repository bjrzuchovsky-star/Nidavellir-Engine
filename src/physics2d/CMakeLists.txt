# ts_physics2d: the core 2D physics *interface* (physics2d::World) and the
# backend registry. Box2D lives in the ts.physics.box2d Trading Post plugin
# (src/plugins/physics_box2d). Used by the 2D World for rigid_body entities
# and by the Shipyard's optional Box2D hulls. Built for every app that runs
# the game.
add_library(ts_physics2d Physics2D.cpp)
target_include_directories(ts_physics2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ts_physics2d PUBLIC cxx_std_20)
