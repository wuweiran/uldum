#version 450

layout(set = 0, binding = 0) uniform sampler2DArray terrain_layers;
layout(set = 0, binding = 1) uniform sampler2D fog_tex;
layout(set = 0, binding = 2) uniform sampler2D transition_noise;
layout(set = 0, binding = 3) uniform sampler2DArray terrain_normals;

layout(set = 1, binding = 0) uniform ShadowData { mat4 light_vp; } shadow;
layout(set = 1, binding = 1) uniform sampler2DShadow shadow_map;
struct PointLight {
    vec4 position;  // xyz = pos, w = radius
    vec4 color;     // rgb = color, a = intensity
};
layout(set = 1, binding = 2) uniform EnvironmentData {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient_color;
    vec4 fog_color;
    ivec4 light_count;  // x = active count
    PointLight lights[8];
} env;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec2 world_size;
    float fog_enabled;
    float time;
} pc;

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) in vec2 frag_tile_uv;
layout(location = 3) in vec3 frag_world_pos;
layout(location = 4) flat in uint frag_layer_corners;
layout(location = 5) flat in uint frag_case_info;
layout(location = 6) in vec2 frag_fog_uv;

layout(location = 0) out vec4 out_color;

float shadow_pcf(vec3 light_ndc) {
    float shadow = 0.0;
    vec2 texel_size = vec2(1.0 / 2048.0);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += texture(shadow_map, vec3(light_ndc.xy + vec2(x,y) * texel_size, light_ndc.z));
        }
    }
    return shadow / 9.0;
}

// Sample diffuse + normal for a layer, return as a pair
struct LayerSample {
    vec3 color;
    vec3 ts_normal;  // tangent-space normal
};

LayerSample sample_layer(uint layer_id) {
    LayerSample s;
    // Scale UV so texture spans 2x2 tiles, reducing visible repetition
    vec2 tex_uv = frag_tile_uv * 0.5;
    s.color = texture(terrain_layers, vec3(tex_uv, float(layer_id))).rgb;
    vec3 n = texture(terrain_normals, vec3(tex_uv, float(layer_id))).rgb;
    s.ts_normal = n * 2.0 - 1.0;  // unpack [0,1] → [-1,1]
    return s;
}

LayerSample mix_samples(LayerSample a, LayerSample b, float t) {
    LayerSample s;
    s.color = mix(a.color, b.color, t);
    s.ts_normal = mix(a.ts_normal, b.ts_normal, t);
    return s;
}

void main() {
    vec3 normal = normalize(frag_world_normal);

    uint c0 = frag_layer_corners & 0xFFu;
    uint c1 = (frag_layer_corners >> 8u) & 0xFFu;
    uint c2 = (frag_layer_corners >> 16u) & 0xFFu;
    uint c3 = (frag_layer_corners >> 24u) & 0xFFu;

    LayerSample result;

    if (c0 == c1 && c1 == c2 && c2 == c3) {
        result = sample_layer(c0);
    } else {
        vec2 uv = fract(frag_tile_uv);
        uint ct[4] = uint[4](c0, c1, c2, c3);

        // Noise perturbation
        float noise = texture(transition_noise, frag_tile_uv).r;
        float perturb = (noise - 0.5) * 0.22;

        // SDF per corner (same perturbation for all — shifts circle radius by noise)
        vec2 cp[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
        float r = 0.5 + perturb;
        float sdf[4];
        sdf[0] = length(uv - cp[0]) - r;
        sdf[1] = length(uv - cp[1]) - r;
        sdf[2] = length(uv - cp[2]) - r;
        sdf[3] = length(uv - cp[3]) - r;

        // Background type (most common)
        uint bg = c0;
        uint bg_count = 0u;
        for (int i = 0; i < 4; i++) {
            uint cnt = 0u;
            for (int j = 0; j < 4; j++) { if (ct[j] == ct[i]) cnt++; }
            if (cnt > bg_count) { bg_count = cnt; bg = ct[i]; }
        }

        // Detect 2-2 edge splits
        bool is_edge_h = (c0 == c1 && c2 == c3 && c0 != c2);
        bool is_edge_v = (c0 == c2 && c1 == c3 && c0 != c1);

        if (is_edge_h) {
            float curve = (1.0 - cos(uv.x * 6.28318)) * 0.5;
            float wave = 0.5 + curve * 0.15 + perturb;
            float t = smoothstep(-0.06, 0.06, uv.y - wave);
            result = mix_samples(sample_layer(c0), sample_layer(c2), t);
        } else if (is_edge_v) {
            float curve = (1.0 - cos(uv.y * 6.28318)) * 0.5;
            float wave = 0.5 + curve * 0.15 + perturb;
            float t = smoothstep(-0.06, 0.06, uv.x - wave);
            result = mix_samples(sample_layer(c0), sample_layer(c1), t);
        } else {
            // SDF quarter-circles
            float best_sdf = 999.0;
            uint best_type = bg;
            for (int i = 0; i < 4; i++) {
                if (ct[i] != bg && sdf[i] < best_sdf) {
                    best_sdf = sdf[i]; best_type = ct[i];
                }
            }
            for (int i = 0; i < 4; i++) {
                if (ct[i] == best_type && sdf[i] < best_sdf) best_sdf = sdf[i];
            }

            if (best_type == bg) {
                result = sample_layer(bg);
            } else {
                float t = 1.0 - smoothstep(-0.06, 0.06, best_sdf);
                result = mix_samples(sample_layer(bg), sample_layer(best_type), t);
            }
        }
    }

    // Apply normal map: transform tangent-space normal to world space
    {
        vec3 T = normalize(normal.z * vec3(1, 0, 0) - normal.x * vec3(0, 0, 1));
        vec3 B = cross(normal, T);
        normal = normalize(T * result.ts_normal.x + B * result.ts_normal.y + normal * result.ts_normal.z);
    }

    // Lighting
    vec3 light_dir = normalize(env.sun_direction.xyz);
    float ndotl = max(dot(normal, light_dir), 0.0);

    vec4 light_clip = shadow.light_vp * vec4(frag_world_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;
    light_ndc.xy = light_ndc.xy * 0.5 + 0.5;

    float shadow_factor = 1.0;
    if (light_ndc.x >= 0.0 && light_ndc.x <= 1.0 &&
        light_ndc.y >= 0.0 && light_ndc.y <= 1.0 &&
        light_ndc.z >= 0.0 && light_ndc.z <= 1.0) {
        shadow_factor = shadow_pcf(light_ndc);
    }

    float ambient = env.ambient_color.a;
    vec3 lit_color = result.color * (env.ambient_color.rgb * ambient
                   + env.sun_color.rgb * (1.0 - ambient) * ndotl * shadow_factor);

    // Point lights
    for (int i = 0; i < env.light_count.x; i++) {
        vec3 lv = env.lights[i].position.xyz - frag_world_pos;
        float d = length(lv);
        float r = env.lights[i].position.w;
        if (d < r) {
            float atten = (1.0 - d / r); atten *= atten;
            float nl = max(dot(normal, normalize(lv)), 0.0);
            lit_color += result.color * env.lights[i].color.rgb * env.lights[i].color.a * nl * atten;
        }
    }

    if (pc.fog_enabled > 0.5) {
        lit_color *= texture(fog_tex, frag_fog_uv).r;
    }

    out_color = vec4(lit_color, 1.0);
}
