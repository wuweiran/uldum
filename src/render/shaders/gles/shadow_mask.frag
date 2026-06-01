#version 310 es
precision highp float;
precision highp int;

// Shadow pass for alphaMode=MASK: sample diffuse alpha and discard
// below the per-instance cutoff so the shadow map captures the
// visible silhouette (clean leaf-shape shadows, not the rect bbox).

// Unit-texture array — same binding (set 1 binding 0 → 16) the main
// pass uses for its bindless replacement.
layout(binding = 16) uniform highp sampler2DArray u_textures;

layout(location = 0)      in vec2  frag_texcoord;
layout(location = 1) flat in uint  frag_material_index;
layout(location = 2)      in vec4  frag_base_color_factor;
layout(location = 3) flat in float frag_alpha_cutoff;

void main() {
    float a = texture(u_textures, vec3(frag_texcoord, float(frag_material_index))).a
            * frag_base_color_factor.a;
    if (a < frag_alpha_cutoff) discard;
}
