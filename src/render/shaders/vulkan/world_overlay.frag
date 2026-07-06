#version 450

// World-overlay fragment shader. Samples the bound texture (alpha mask
// for the shape, e.g. ring stroke / circle / curve / cone) and
// multiplies by the per-vertex × push-constant tint. Both texture and
// vertex color are stored in premultiplied-alpha form, so the multiply
// is component-wise and the result feeds straight into the consumer's
// premultiplied-alpha blend (ONE * src + (1-src.a) * dst).
//
// The texture is per-draw — descriptor set 0 binding 0 is rebound
// between draws to switch shape mask. Texture is RGBA so a future
// authored decal can carry color directly; the runtime-generated
// defaults store (a, a, a, a) so the multiply collapses to a pure
// alpha mask of the vertex color.

layout(set = 0, binding = 0) uniform sampler2D u_tex;

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
