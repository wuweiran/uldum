#version 450

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec3 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 normal = normalize(frag_world_normal);

    // Directional sun light (game coords: X=right, Y=forward, Z=up)
    // Light from upper-right, slightly behind camera
    vec3 light_dir = normalize(vec3(0.3, -0.5, 0.8));
    float ndotl = max(dot(normal, light_dir), 0.0);

    float ambient = 0.25;
    float lighting = ambient + (1.0 - ambient) * ndotl;

    out_color = vec4(frag_color * lighting, 1.0);
}
