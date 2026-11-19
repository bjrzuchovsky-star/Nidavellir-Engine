#version 450
#extension GL_GOOGLE_include_directive : require
#include "samplers.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = texture(u_albedo_tex, v_uv); }
