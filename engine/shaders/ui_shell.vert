#version 450

// Shell UI (RmlUi) vertex shader.
// RmlUi's Vertex layout: vec2 position, ubyte4 colour (premultiplied alpha),
// vec2 tex_coord. We bake the orthographic projection + per-draw transform
// + translation into one mat4 pushed as a push constant.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_color;   // u8×4 premultiplied → normalized float
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform PC {
    mat4 mvp;     // ortho(w, h) * transform * translate
} pc;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 0.0, 1.0);
    out_color   = in_color;
    out_uv      = in_uv;
}
