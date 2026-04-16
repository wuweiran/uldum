#version 450

layout(set = 0, binding = 0) uniform sampler2DArray terrain_layers;
layout(set = 0, binding = 1) uniform sampler2D fog_tex;
layout(set = 0, binding = 2) uniform sampler2D transition_noise;
layout(set = 0, binding = 3) uniform sampler2DArray terrain_normals;
layout(set = 0, binding = 4) uniform sampler2D water_normal_map;

layout(set = 1, binding = 2) uniform EnvironmentData {
    vec4 sun_direction;
    vec4 sun_color;
    vec4 ambient_color;
    vec4 fog_color;
} env;
layout(set = 1, binding = 3) uniform samplerCube env_cubemap;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec2 world_size;
    float fog_enabled;
    float time;
    // Water-specific (offset 144)
    vec4  shallow_color;   // rgb (a unused)
    vec4  deep_color;      // rgb (a unused)
    uint  water_mask;      // bitmask: layer IDs that are water
    uint  deep_mask;       // bitmask: deep water layers (rendered opaque)
} pc;

layout(location = 0) in vec3 frag_world_normal;
layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) in vec2 frag_tile_uv;
layout(location = 3) in vec3 frag_world_pos;
layout(location = 4) flat in uint frag_layer_corners;
layout(location = 5) flat in uint frag_case_info;
layout(location = 6) in vec2 frag_fog_uv;

layout(location = 0) out vec4 out_color;

float is_water(uint layer_id) {
    return ((pc.water_mask & (1u << layer_id)) != 0u) ? 1.0 : 0.0;
}

bool is_deep(uint layer_id) {
    return (pc.deep_mask & (1u << layer_id)) != 0u;
}

void main() {
    uint c0 = frag_layer_corners & 0xFFu;
    uint c1 = (frag_layer_corners >> 8u) & 0xFFu;
    uint c2 = (frag_layer_corners >> 16u) & 0xFFu;
    uint c3 = (frag_layer_corners >> 24u) & 0xFFu;

    float w0 = is_water(c0), w1 = is_water(c1), w2 = is_water(c2), w3 = is_water(c3);
    float water_sum = w0 + w1 + w2 + w3;

    // No water on this tile at all
    if (water_sum == 0.0) discard;

    float d0 = is_deep(c0) ? 1.0 : 0.0;
    float d1 = is_deep(c1) ? 1.0 : 0.0;
    float d2 = is_deep(c2) ? 1.0 : 0.0;
    float d3 = is_deep(c3) ? 1.0 : 0.0;

    // Whether any corner is deep — for choosing noise at water/land edges
    bool has_deep = (d0 + d1 + d2 + d3) > 0.0;
    // Whether this tile has non-water corners (water/land boundary)
    bool has_land = water_sum < 4.0;

    // Compute water alpha + deep_blend (0=shallow, 1=deep) from same blending
    float water_alpha;
    float deep_blend;

    if (c0 == c1 && c1 == c2 && c2 == c3) {
        // Uniform tile
        water_alpha = 1.0;
        deep_blend = d0;
    } else {
        vec2 uv = fract(frag_tile_uv);
        uint ct[4] = uint[4](c0, c1, c2, c3);

        // When deep water touches land: match terrain noise exactly.
        // When only shallow water touches land: puddle-like edges.
        // When only water types meet (no land): match terrain noise.
        float noise, perturb;
        if (has_deep || !has_land) {
            noise = texture(transition_noise, frag_tile_uv).r;
            perturb = (noise - 0.5) * 0.22;  // identical to terrain.frag
        } else {
            noise = texture(transition_noise, frag_tile_uv * 1.4 + vec2(0.37, 0.71)).r;
            perturb = (noise - 0.5) * 0.28;
        }

        vec2 cp[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
        float r = 0.5 + perturb;
        float sdf[4];
        sdf[0] = length(uv - cp[0]) - r;
        sdf[1] = length(uv - cp[1]) - r;
        sdf[2] = length(uv - cp[2]) - r;
        sdf[3] = length(uv - cp[3]) - r;

        // Background type — same logic as terrain.frag
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

        // Edge style: deep/land → terrain-matching, shallow-only/land → puddle
        float edge_freq = (has_deep || !has_land) ? 6.28318 : 12.56637;
        float edge_amp = (has_deep || !has_land) ? 0.15 : 0.12;
        float edge_smooth = (has_deep || !has_land) ? 0.06 : 0.05;

        if (is_edge_h) {
            float curve = (1.0 - cos(uv.x * edge_freq)) * 0.5;
            float wave = 0.5 + curve * edge_amp + perturb;
            float t = smoothstep(-edge_smooth, edge_smooth, uv.y - wave);
            water_alpha = mix(w0, w2, t);
            deep_blend = mix(d0, d2, t);
        } else if (is_edge_v) {
            float curve = (1.0 - cos(uv.y * edge_freq)) * 0.5;
            float wave = 0.5 + curve * edge_amp + perturb;
            float t = smoothstep(-edge_smooth, edge_smooth, uv.x - wave);
            water_alpha = mix(w0, w1, t);
            deep_blend = mix(d0, d1, t);
        } else {
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

            float bg_deep = is_deep(bg) ? 1.0 : 0.0;
            float fg_deep = is_deep(best_type) ? 1.0 : 0.0;

            if (best_type == bg) {
                water_alpha = is_water(bg);
                deep_blend = bg_deep;
            } else {
                float sdf_smooth = (has_deep || !has_land) ? 0.06 : 0.05;
                float t = 1.0 - smoothstep(-sdf_smooth, sdf_smooth, best_sdf);
                water_alpha = mix(is_water(bg), is_water(best_type), t);
                deep_blend = mix(bg_deep, fg_deep, t);
            }
        }
    }

    // Shore strip: only at water/land boundaries with no deep water.
    // Deep water and shallow/deep transitions don't get shore shrink.
    if (has_land && !has_deep) {
        float shore = 0.3;
        water_alpha = smoothstep(shore, shore + 0.15, water_alpha);
    }

    if (water_alpha < 0.01) discard;

    // Blend color between shallow and deep based on smooth deep_blend
    vec3 tint_color = mix(pc.shallow_color.rgb, pc.deep_color.rgb, deep_blend);

    vec2 wpos = frag_world_pos.xy;
    float wt = pc.time;
    vec3 water_n;
    float wh = 0.0;
    float pnoise = texture(transition_noise, wpos * 0.002).r * 6.28;

    if (deep_blend < 0.5) {
        // ── Shallow: hash noise normal map, two scrolling layers ─────────
        vec2 uv1 = wpos * 0.0008 + wt * vec2(0.006, 0.0035);
        vec2 uv2 = wpos * 0.0013 + wt * vec2(-0.0045, 0.0055);
        vec3 n1 = texture(water_normal_map, uv1).rgb * 2.0 - 1.0;
        vec3 n2 = texture(water_normal_map, uv2).rgb * 2.0 - 1.0;
        water_n = normalize(vec3(n1.xy + n2.xy, n1.z + n2.z));
        wh = (n1.x + n1.y + n2.x + n2.y) * 0.25;
    } else {
        // ── Deep: Gerstner waves (GPU Gems Ch.1) ────────────────────────
        float nx = 0.0, ny = 0.0, nz = 0.0;

        // Wave 1: broad swell — very sparse
        {
            vec2 D = normalize(vec2(0.85, 0.5));
            float w = 0.012, A = 0.8, phi = 0.5, Q = 0.85;
            float phase = dot(D, wpos) * w + wt * phi + pnoise;
            float S = sin(phase), C = cos(phase);
            nx += D.x * w * A * C; ny += D.y * w * A * C; nz += Q * w * A * S; wh += A * S;
        }
        // Wave 2: medium cross wave
        {
            vec2 D = normalize(vec2(-0.5, 0.87));
            float w = 0.03, A = 0.5, phi = 0.9, Q = 0.75;
            float phase = dot(D, wpos) * w + wt * phi + pnoise * 0.8;
            float S = sin(phase), C = cos(phase);
            nx += D.x * w * A * C; ny += D.y * w * A * C; nz += Q * w * A * S; wh += A * S;
        }
        // Wave 3: fine ripple
        {
            vec2 D = normalize(vec2(0.3, -0.95));
            float w = 0.06, A = 0.3, phi = 1.5, Q = 0.6;
            float phase = dot(D, wpos) * w + wt * phi + pnoise * 1.1;
            float S = sin(phase), C = cos(phase);
            nx += D.x * w * A * C; ny += D.y * w * A * C; nz += Q * w * A * S; wh += A * S;
        }
        water_n = normalize(vec3(-nx, -ny, 1.0 - nz));
    }

    // ── Specular highlights ─────────────────────────────────────────────
    vec3 light_dir = normalize(env.sun_direction.xyz);
    vec3 view_dir = normalize(vec3(0.0, -0.6, 1.0));
    vec3 surface_normal = normalize(vec3(water_n.xy * 0.6, 1.0));
    vec3 half_vec = normalize(light_dir + view_dir);
    float ndoth = max(dot(surface_normal, half_vec), 0.0);
    float spec = pow(ndoth, 48.0);

    // Wave crest tint
    float wave_intensity = max(water_n.x + water_n.y, 0.0) * 0.5;

    // Cubemap reflection: reflect view off water surface
    vec3 reflected = reflect(-view_dir, surface_normal);
    vec3 env_reflect = texture(env_cubemap, reflected).rgb;

    // Fog
    float fog = 1.0;
    if (pc.fog_enabled > 0.5) {
        fog = texture(fog_tex, frag_fog_uv).r;
    }

    // Shallow: crystal clear + wave-varying reflection
    float reflect_strength = 0.1 + wave_intensity * 0.3;  // stronger on crests
    vec3 shallow_color = mix(tint_color, env_reflect, reflect_strength) + vec3(spec * 0.3);
    float shallow_alpha = (spec * 0.8 + wave_intensity * 0.4 + 0.1) * water_alpha;

    // Fresnel edge brightening: thin water at shore catches more light
    float fresnel = (1.0 - smoothstep(0.0, 0.5, water_alpha)) * water_alpha * 2.0;
    shallow_color = mix(shallow_color, vec3(0.85, 0.9, 0.95), fresnel * 0.6);
    shallow_alpha = clamp(shallow_alpha + fresnel * 0.3, 0.0, 1.0);

    // Noise-driven shade: organic base variation
    float shade1 = texture(transition_noise, wpos * 0.001 + wt * vec2(0.004, 0.003)).r;
    float shade2 = texture(transition_noise, wpos * 0.0015 + wt * vec2(-0.003, 0.005)).r;
    float shade = (shade1 + shade2) - 1.0;

    // 1 Gerstner wave with noise-modulated amplitude (patchy, not uniform)
    // and strong phase perturbation (bends the ridge)
    float amp_mod = texture(transition_noise, wpos * 0.0008 + wt * vec2(0.002, 0.001)).r;
    {
        vec2 D = normalize(vec2(0.8, 0.55));
        float w = 0.015, A = 0.6 * amp_mod, phi = 0.5, Q = 0.8;
        float phase = dot(D, wpos) * w + wt * phi + pnoise * 2.0;
        float S = sin(phase);
        wh += A * S;
    }

    // Deep: base color + noise shade + Gerstner + wave-varying reflection
    vec3 deep_base = tint_color * (1.0 + shade * 0.35 + wh * 0.25);
    float deep_reflect = 0.1 + wave_intensity * 0.35;  // stronger on crests
    vec3 deep_color = mix(deep_base, env_reflect, deep_reflect) + vec3(spec * 0.15);
    float deep_alpha = water_alpha;

    // Smooth blend between shallow and deep
    vec3 final_color = mix(shallow_color, deep_color, deep_blend);
    float final_alpha = mix(shallow_alpha, deep_alpha, deep_blend);

    out_color = vec4(final_color * fog, final_alpha * fog);
}
