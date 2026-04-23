#version 450

// Selection circle ring — one vertex per ring-local position, transformed
// per draw by a push-constant MVP. Alpha-blended color also in the push
// constant so each drawn unit gets its own color without touching
// descriptors. Pre-generated mesh sits in the XY plane (z=0) around the
// origin; mvp places + scales it in world space.

layout(location = 0) in vec3 in_pos;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    out_color   = pc.color;
}
