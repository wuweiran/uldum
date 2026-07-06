#version 310 es
precision highp float;

// set 0 binding 0 → flat 0
layout(binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

// Vertex colors are authored in sRGB but arrive via an R8G8B8A8_UNORM
// attribute (no hardware decode). Decode to linear so the sRGB swapchain's
// encode-on-store round-trips the authored value instead of brightening it.
vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main() {
    vec4 c = in_color;
    c.rgb = srgb_to_linear(c.rgb);
    out_color = texture(u_tex, in_uv) * c;
}
