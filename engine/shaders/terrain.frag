#version 450

layout(set = 0, binding = 0) uniform sampler2D layer0_tex;  // grass
layout(set = 0, binding = 1) uniform sampler2D layer1_tex;  // dirt
layout(set = 0, binding = 2) uniform sampler2D layer2_tex;  // stone
layout(set = 0, binding = 3) uniform sampler2D layer3_tex;  // sand
layout(set = 0, binding = 4) uniform sampler2D splatmap_tex; // RGBA blend weights

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) in vec2 frag_tile_uv;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 normal = normalize(frag_world_normal);

    // Sample splatmap at terrain UV (0-1 across whole terrain)
    vec4 splat = texture(splatmap_tex, frag_texcoord);

    // Sample each ground layer at tiling UV (repeating per tile)
    vec3 c0 = texture(layer0_tex, frag_tile_uv).rgb;
    vec3 c1 = texture(layer1_tex, frag_tile_uv).rgb;
    vec3 c2 = texture(layer2_tex, frag_tile_uv).rgb;
    vec3 c3 = texture(layer3_tex, frag_tile_uv).rgb;

    // Blend layers using splatmap weights (R=layer0, G=layer1, B=layer2, A=layer3)
    vec3 albedo = c0 * splat.r + c1 * splat.g + c2 * splat.b + c3 * splat.a;

    // Directional sun light (game coords: X=right, Y=forward, Z=up)
    vec3 light_dir = normalize(vec3(0.3, -0.5, 0.8));
    float ndotl = max(dot(normal, light_dir), 0.0);

    float ambient = 0.25;
    float lighting = ambient + (1.0 - ambient) * ndotl;

    out_color = vec4(albedo * lighting, 1.0);
}
