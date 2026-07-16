#version 450

// Model-viewer fragment (shared by static + skinned). Self-contained lighting:
// two fixed world-space directional lights (key + fill from opposite sides) +
// ambient, so the model reads well at any orbit angle and looks identical
// regardless of scene state. No shadow map, no scene env — full isolation.

layout(set = 0, binding = 0) uniform sampler2D diffuse_tex;

layout(push_constant) uniform PushConstants {
    layout(offset = 128) vec4 base_color;
} pc;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex = texture(diffuse_tex, frag_uv);
    if (tex.a < 0.5) discard;                      // MASK cut-outs; opaque passes (a=1)

    vec3 n = normalize(frag_normal);
    const vec3 KEY  = normalize(vec3( 0.4, 0.6, 0.7));
    const vec3 FILL = normalize(vec3(-0.5, -0.4, 0.3));
    float lit = 0.35                               // ambient floor
              + 0.65 * max(dot(n, KEY), 0.0)
              + 0.25 * max(dot(n, FILL), 0.0);

    vec3 rgb = tex.rgb * pc.base_color.rgb * lit;
    out_color = vec4(rgb, 1.0);
}
