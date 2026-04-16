#version 450

// Fragment shader for skinned meshes — single diffuse texture per model.

// Set 0: material textures
layout(set = 0, binding = 0) uniform sampler2D diffuse_tex;

// Set 1: shadow data
layout(set = 1, binding = 0) uniform ShadowData {
    mat4 light_vp;
} shadow;
layout(set = 1, binding = 1) uniform sampler2DShadow shadow_map;
struct PointLight {
    vec4 position;
    vec4 color;
};
layout(set = 1, binding = 2) uniform EnvironmentData {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient_color;
    vec4 fog_color;
    ivec4 light_count;
    PointLight lights[8];
} env;

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) in vec3 frag_world_pos;

layout(location = 0) out vec4 out_color;

float shadow_pcf(vec3 light_ndc) {
    // 3x3 PCF kernel
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

    // Sample diffuse texture
    vec3 albedo = texture(diffuse_tex, frag_texcoord).rgb;

    // Directional sun light (game coords: X=right, Y=forward, Z=up)
    vec3 light_dir = normalize(env.sun_direction.xyz);
    float ndotl = max(dot(normal, light_dir), 0.0);

    // Shadow
    vec4 light_clip = shadow.light_vp * vec4(frag_world_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;
    // Map from [-1,1] to [0,1] for UV lookup
    light_ndc.xy = light_ndc.xy * 0.5 + 0.5;

    float shadow_factor = 1.0;
    if (light_ndc.x >= 0.0 && light_ndc.x <= 1.0 &&
        light_ndc.y >= 0.0 && light_ndc.y <= 1.0 &&
        light_ndc.z >= 0.0 && light_ndc.z <= 1.0) {
        shadow_factor = shadow_pcf(light_ndc);
    }

    float ambient = env.ambient_color.a;
    vec3 lighting = env.ambient_color.rgb * ambient
                  + env.sun_color.rgb * (1.0 - ambient) * ndotl * shadow_factor;

    vec3 lit = albedo * lighting;

    for (int i = 0; i < env.light_count.x; i++) {
        vec3 lv = env.lights[i].position.xyz - frag_world_pos;
        float d = length(lv);
        float r = env.lights[i].position.w;
        if (d < r) {
            float atten = (1.0 - d / r); atten *= atten;
            float nl = max(dot(normal, normalize(lv)), 0.0);
            lit += albedo * env.lights[i].color.rgb * env.lights[i].color.a * nl * atten;
        }
    }

    out_color = vec4(lit, 1.0);
}
