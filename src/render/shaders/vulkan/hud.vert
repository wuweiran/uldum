#version 450

// HUD (custom retained-mode UI) vertex shader.
// Shared layout: position (vec2), color (u8×4 premultiplied → normalized
// vec4), uv (vec2). Used for both solid-color quads (sampled against a 1×1
// white texture) and textured quads (icons, MSDF glyphs).
//
// Orthographic projection is passed as a push constant. Screen-space coords
// are top-left origin, y-down; the ortho matrix maps (0, 0)..(w, h) to
// Vulkan NDC.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform PC {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 0.0, 1.0);
    out_color   = in_color;
    out_uv      = in_uv;
}
