#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec3 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    // Simple directional light
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    float ndotl = max(dot(normalize(frag_normal), light_dir), 0.0);
    float ambient = 0.3;
    float lighting = ambient + (1.0 - ambient) * ndotl;

    out_color = vec4(frag_color * lighting, 1.0);
}
