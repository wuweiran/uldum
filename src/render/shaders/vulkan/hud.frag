#version 450

// HUD fragment shader.
// Premultiplied-alpha blending (pipeline sets ONE + ONE_MINUS_SRC_ALPHA).
// Non-textured quads bind a 1×1 white texture so this shader has a single
// path; textured quads (icons, later MSDF glyphs) bind their own image.

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

// Vertex colors are authored in sRGB (byte 0..255) but arrive via an
// R8G8B8A8_UNORM attribute, so they normalize linearly with no decode.
// Decode to linear here so the sRGB swapchain's encode-on-store round-trips
// the authored value instead of brightening it (see world/UI gamma note).
vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main() {
    vec4 c = in_color;
    c.rgb = srgb_to_linear(c.rgb);
    out_color = c * texture(u_tex, in_uv);
}
