#version 450

// Shell UI (RmlUi) fragment shader.
// Premultiplied-alpha blending — color × texture sample, pipeline sets
// blend equation to ONE + ONE_MINUS_SRC_ALPHA.
//
// RmlUi uses a 1×1 white texture for untextured geometry so this shader
// has a single path; no branching needed.

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color * texture(u_tex, in_uv);
}
