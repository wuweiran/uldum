#version 450

// HUD fragment shader.
// Premultiplied-alpha blending (pipeline sets ONE + ONE_MINUS_SRC_ALPHA).
// Non-textured quads bind a 1×1 white texture so this shader has a single
// path; textured quads (icons, later MSDF glyphs) bind their own image.

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color * texture(u_tex, in_uv);
}
