#version 450

layout(set = 0, binding = 0) uniform sampler2DArray terrain_layers;
layout(set = 0, binding = 1) uniform sampler2D fog_tex;
layout(set = 0, binding = 2) uniform sampler2D transition_noise;

layout(set = 1, binding = 0) uniform ShadowData { mat4 light_vp; } shadow;
layout(set = 1, binding = 1) uniform sampler2DShadow shadow_map;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec2 world_size;
    float fog_enabled;
    float _pad;
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

void main() {
    vec3 normal = normalize(frag_world_normal);

    uint c0 = frag_layer_corners & 0xFFu;
    uint c1 = (frag_layer_corners >> 8u) & 0xFFu;
    uint c2 = (frag_layer_corners >> 16u) & 0xFFu;
    uint c3 = (frag_layer_corners >> 24u) & 0xFFu;

    vec3 albedo;

    if (c0 == c1 && c1 == c2 && c2 == c3) {
        albedo = texture(terrain_layers, vec3(frag_tile_uv, float(c0))).rgb;
    } else {
        vec2 uv = fract(frag_tile_uv);
        uint ct[4] = uint[4](c0, c1, c2, c3);
        vec2 cp[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));

        // Noise perturbation for organic curve variation
        vec2 tile_id = floor(frag_tile_uv);
        float hash = fract(sin(dot(tile_id, vec2(127.1, 311.7))) * 43758.5453);
        float hash2 = fract(sin(dot(tile_id, vec2(269.5, 183.3))) * 43758.5453);
        vec2 noise_uv = frag_tile_uv + vec2(hash, hash2);
        float noise = texture(transition_noise, noise_uv).r;
        float perturb = (noise - 0.5) * 0.22;

        // SDF: distance to each corner's circle (radius 0.5)
        float sdf[4];
        sdf[0] = length(uv - cp[0]) - 0.5 + perturb;
        sdf[1] = length(uv - cp[1]) - 0.5 - perturb;
        sdf[2] = length(uv - cp[2]) - 0.5 - perturb * 0.8;
        sdf[3] = length(uv - cp[3]) - 0.5 + perturb * 0.8;

        // Find background type (most common)
        uint bg = c0;
        uint bg_count = 0u;
        for (int i = 0; i < 4; i++) {
            uint cnt = 0u;
            for (int j = 0; j < 4; j++) { if (ct[j] == ct[i]) cnt++; }
            if (cnt > bg_count) { bg_count = cnt; bg = ct[i]; }
        }

        // Detect 2-2 edge split: use sine wave boundary instead of circles
        bool is_edge_h = (c0 == c1 && c2 == c3 && c0 != c2);  // horizontal split
        bool is_edge_v = (c0 == c2 && c1 == c3 && c0 != c1);  // vertical split

        if (is_edge_h) {
            // Horizontal split: c0,c1 on bottom (y=0), c2,c3 on top (y=1)
            // C1-continuous: (1-cos(2πx))/2 has zero value AND zero derivative at x=0,1
            float curve = (1.0 - cos(uv.x * 6.28318)) * 0.5;
            float wave = 0.5 + curve * 0.15 + perturb * 0.3;
            float d = uv.y - wave;
            float t = smoothstep(-0.06, 0.06, d);
            vec3 col_bot = texture(terrain_layers, vec3(frag_tile_uv, float(c0))).rgb;
            vec3 col_top = texture(terrain_layers, vec3(frag_tile_uv, float(c2))).rgb;
            albedo = mix(col_bot, col_top, t);
        } else if (is_edge_v) {
            // Vertical split: c0,c2 on left (x=0), c1,c3 on right (x=1)
            float curve = (1.0 - cos(uv.y * 6.28318)) * 0.5;
            float wave = 0.5 + curve * 0.15 + perturb * 0.3;
            float d = uv.x - wave;
            float t = smoothstep(-0.06, 0.06, d);
            vec3 col_left = texture(terrain_layers, vec3(frag_tile_uv, float(c0))).rgb;
            vec3 col_right = texture(terrain_layers, vec3(frag_tile_uv, float(c1))).rgb;
            albedo = mix(col_left, col_right, t);
        } else {
            // All other cases: SDF quarter-circles per non-background corner
            float best_sdf = 999.0;
            uint best_type = bg;
            for (int i = 0; i < 4; i++) {
                if (ct[i] != bg && sdf[i] < best_sdf) {
                    best_sdf = sdf[i];
                    best_type = ct[i];
                }
            }
            // Merge same-type circles (union)
            for (int i = 0; i < 4; i++) {
                if (ct[i] == best_type && sdf[i] < best_sdf) {
                    best_sdf = sdf[i];
                }
            }

            vec3 bg_color = texture(terrain_layers, vec3(frag_tile_uv, float(bg))).rgb;
            if (best_type == bg) {
                albedo = bg_color;
            } else {
                vec3 fg_color = texture(terrain_layers, vec3(frag_tile_uv, float(best_type))).rgb;
                float t = 1.0 - smoothstep(-0.06, 0.06, best_sdf);
                albedo = mix(bg_color, fg_color, t);
            }
        }
    }

    // Lighting
    vec3 light_dir = normalize(vec3(0.3, -0.5, 0.8));
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

    float ambient = 0.25;
    float lighting = ambient + (1.0 - ambient) * ndotl * shadow_factor;
    vec3 lit_color = albedo * lighting;

    if (pc.fog_enabled > 0.5) {
        lit_color *= texture(fog_tex, frag_fog_uv).r;
    }

    out_color = vec4(lit_color, 1.0);
}
