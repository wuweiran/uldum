#version 450

layout(set = 0, binding = 0) uniform sampler2D diffuse_tex;

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec2 frag_texcoord;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 normal = normalize(frag_world_normal);

    // Sample diffuse texture
    vec3 albedo = texture(diffuse_tex, frag_texcoord).rgb;

    // Directional sun light (game coords: X=right, Y=forward, Z=up)
    vec3 light_dir = normalize(vec3(0.3, -0.5, 0.8));
    float ndotl = max(dot(normal, light_dir), 0.0);

    float ambient = 0.25;
    float lighting = ambient + (1.0 - ambient) * ndotl;

    out_color = vec4(albedo * lighting, 1.0);
}
