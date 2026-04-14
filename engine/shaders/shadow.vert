#version 450

// Shadow depth pass with instancing.
// Push constant holds light_vp (same for all instances).
// Instance buffer holds per-entity model matrices.

layout(push_constant) uniform PushConstants {
    mat4 light_vp;
} pc;

layout(set = 0, binding = 0) readonly buffer InstanceBuffer {
    mat4 models[];
} instances;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;    // unused but must match vertex layout
layout(location = 2) in vec2 in_texcoord;  // unused but must match vertex layout

void main() {
    mat4 model = instances.models[gl_InstanceIndex];
    gl_Position = pc.light_vp * model * vec4(in_position, 1.0);
}
