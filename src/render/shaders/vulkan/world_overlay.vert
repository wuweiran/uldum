#version 450

// World-overlay shader. Used by everything in the WorldOverlays family
// — selection circles, ability targeting indicators, future
// build-placement ghosts, debug gizmos. Vertex carries world-space
// position, UV, and a packed RGBA8 color (premultiplied alpha). The
// fragment samples a texture (per-draw bound) and multiplies by the
// vertex color × push-constant tint, so the visual identity comes from
// the texture and the per-draw style is just color/dimensions.
//
// Vertex layout matches the cpp `Vertex` struct:
//   vec3 in_pos        — location 0 — 12 bytes
//   vec2 in_uv         — location 1 —  8 bytes
//   uint in_color_rgba — location 2 —  4 bytes  (packed 8-bit RGBA, premultiplied)

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 tint;       // multiplied component-wise after vertex color
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;   // RGBA8 unorm input

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    out_uv      = in_uv;
    out_color   = pc.tint * in_color;
}
