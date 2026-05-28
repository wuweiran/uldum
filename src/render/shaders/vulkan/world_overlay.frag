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

void main() {
    out_color = texture(u_tex, in_uv) * in_color;
}
