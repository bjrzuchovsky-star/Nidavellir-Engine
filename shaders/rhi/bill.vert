#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;
void main() {
  v_col = a_col;
  v_uv = a_uv;
  gl_Position = ts_clip(frame.view_proj * vec4(a_pos, 1.0));
}
