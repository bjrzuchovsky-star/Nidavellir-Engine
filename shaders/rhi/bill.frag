#version 450
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  float a = clamp(1.0 - length(v_uv * 2.0 - 1.0), 0.0, 1.0);
  a = a * a * (3.0 - 2.0 * a);
  o_color = vec4(v_col.rgb, v_col.a * a);
}
