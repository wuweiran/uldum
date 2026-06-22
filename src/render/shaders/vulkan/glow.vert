#version 450

// Glow vertex: CPU generates camera-aligned vertical shaft quads in world
// space. Static (no motion). `fade` (the 0→1→0 life envelope) is passed
// separately so the fragment shader can retract the scattering halo as the
// glow dies, instead of a flat alpha dim. Push constant is the view-proj.

layout(push_constant) uniform PushConstants {
    mat4 vp;
} pc;

layout(location = 0) in vec3  in_position;
layout(location = 1) in vec4  in_color;
layout(location = 2) in vec2  in_texcoord;
layout(location = 3) in float in_tyndall;
layout(location = 4) in float in_fade;

layout(location = 0) out vec4  frag_color;
layout(location = 1) out vec2  frag_uv;
layout(location = 2) out float frag_tyndall;
layout(location = 3) out float frag_fade;

void main() {
    gl_Position  = pc.vp * vec4(in_position, 1.0);
    frag_color   = in_color;
    frag_uv      = in_texcoord;
    frag_tyndall = in_tyndall;
    frag_fade    = in_fade;
}
