#version 450

layout(push_constant) uniform PushConstants {
    mat4 vp_no_translate;  // view-proj with translation stripped
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 0) out vec3 frag_dir;

void main() {
    frag_dir = in_position;
    vec4 pos = pc.vp_no_translate * vec4(in_position, 1.0);
    gl_Position = pos.xyww;  // z = w → depth = 1.0 (far plane)
}
