#version 450

// Set 0: terrain material textures
layout(set = 0, binding = 0) uniform sampler2D layer0_tex;
layout(set = 0, binding = 1) uniform sampler2D layer1_tex;
layout(set = 0, binding = 2) uniform sampler2D layer2_tex;
layout(set = 0, binding = 3) uniform sampler2D layer3_tex;
layout(set = 0, binding = 4) uniform sampler2D splatmap_tex;

// Set 1: shadow data
layout(set = 1, binding = 0) uniform ShadowData {
    mat4 light_vp;
} shadow;
layout(set = 1, binding = 1) uniform sampler2DShadow shadow_map;

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) in vec2 frag_tile_uv;
layout(location = 3) in vec3 frag_world_pos;

layout(location = 0) out vec4 out_color;

float shadow_pcf(vec3 light_ndc) {
    float shadow = 0.0;
    vec2 texel_size = vec2(1.0 / 2048.0);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(x, y) * texel_size;
            shadow += texture(shadow_map, vec3(light_ndc.xy + offset, light_ndc.z));
        }
    }
    return shadow / 9.0;
}

void main() {
    vec3 normal = normalize(frag_world_normal);

    vec4 splat = texture(splatmap_tex, frag_texcoord);
    vec3 c0 = texture(layer0_tex, frag_tile_uv).rgb;
    vec3 c1 = texture(layer1_tex, frag_tile_uv).rgb;
    vec3 c2 = texture(layer2_tex, frag_tile_uv).rgb;
    vec3 c3 = texture(layer3_tex, frag_tile_uv).rgb;
    vec3 albedo = c0 * splat.r + c1 * splat.g + c2 * splat.b + c3 * splat.a;

    // Directional sun light (game coords: X=right, Y=forward, Z=up)
    vec3 light_dir = normalize(vec3(0.3, -0.5, 0.8));
    float ndotl = max(dot(normal, light_dir), 0.0);

    // Shadow
    vec4 light_clip = shadow.light_vp * vec4(frag_world_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;
    light_ndc.xy = light_ndc.xy * 0.5 + 0.5;

    float shadow_factor = 1.0;
    if (light_ndc.x >= 0.0 && light_ndc.x <= 1.0 &&
        light_ndc.y >= 0.0 && light_ndc.y <= 1.0 &&
        light_ndc.z >= 0.0 && light_ndc.z <= 1.0) {
        shadow_factor = shadow_pcf(light_ndc);
    }

    float ambient = 0.25;
    float lighting = ambient + (1.0 - ambient) * ndotl * shadow_factor;

    out_color = vec4(albedo * lighting, 1.0);
}
