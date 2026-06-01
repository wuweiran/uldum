#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Shadow pass for alphaMode=MASK: sample the diffuse alpha and discard
// fragments below the per-instance cutoff so the shadow map only
// captures the visible silhouette (clean leaf-shape shadows, not the
// rectangular bounding box).

// Set 1 = bindless textures (shadow pass binds it at slot 1 because
// slot 0 is the instance SSBO).
layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0)      in vec2  frag_texcoord;
layout(location = 1) flat in uint  frag_material_index;
layout(location = 2)      in vec4  frag_base_color_factor;
layout(location = 3) flat in float frag_alpha_cutoff;

void main() {
    float a = texture(textures[nonuniformEXT(frag_material_index)], frag_texcoord).a
            * frag_base_color_factor.a;
    if (a < frag_alpha_cutoff) discard;
}
