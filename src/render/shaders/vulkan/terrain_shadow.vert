#version 450

// Terrain shadow depth pass: depth-only render of the terrain mesh
// against the light frustum. Only `in_position` is needed — the
// pipeline binds the full TerrainVertex VBO, but we only sample
// location 0 from offset 0. Other terrain attributes (normal,
// texcoord, layer_corners, case_info) are deliberately not declared
// here; the pipeline matches by only listing location 0 too, so
// validation is happy and no R32_UINT-vs-vec4 layout drift can recur.

layout(push_constant) uniform PushConstants {
    mat4 light_mvp;
} pc;

layout(location = 0) in vec3 in_position;

void main() {
    gl_Position = pc.light_mvp * vec4(in_position, 1.0);
}
