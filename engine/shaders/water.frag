#version 450

layout(set = 0, binding = 0) uniform sampler2DArray terrain_layers;
layout(set = 0, binding = 1) uniform sampler2D fog_tex;
layout(set = 0, binding = 2) uniform sampler2D transition_noise;
layout(set = 0, binding = 3) uniform sampler2DArray terrain_normals;
layout(set = 0, binding = 4) uniform sampler2D water_normal_map;

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

    // No water on this tile at all
    if (w0 + w1 + w2 + w3 == 0.0) discard;

    // Compute water alpha by mirroring exact terrain blending logic
    float water_alpha;
    bool any_deep = false;

    if (c0 == c1 && c1 == c2 && c2 == c3) {
        // Uniform tile
        water_alpha = 1.0;
        any_deep = is_deep(c0);
    } else {
        vec2 uv = fract(frag_tile_uv);
        uint ct[4] = uint[4](c0, c1, c2, c3);

        // Water uses its OWN noise — different UV so the water edge
        // follows a distinct curve from the terrain blend.
        float noise = texture(transition_noise, frag_tile_uv * 1.4 + vec2(0.37, 0.71)).r;
        float perturb = (noise - 0.5) * 0.28;

        // Base radius must be 0.5 so adjacent quarter-circles meet at tile edges.
        // Different noise perturbation gives a distinct curve shape.
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

        if (is_edge_h) {
            // Different curve shape: 4pi = two bumps per tile (terrain uses 2pi = one)
            float curve = (1.0 - cos(uv.x * 12.56637)) * 0.5;
            float wave = 0.5 + curve * 0.12 + perturb;
            float t = smoothstep(-0.05, 0.05, uv.y - wave);
            water_alpha = mix(w0, w2, t);
            any_deep = is_deep(t < 0.5 ? c0 : c2);
        } else if (is_edge_v) {
            float curve = (1.0 - cos(uv.y * 12.56637)) * 0.5;
            float wave = 0.5 + curve * 0.12 + perturb;
            float t = smoothstep(-0.05, 0.05, uv.x - wave);
            water_alpha = mix(w0, w1, t);
            any_deep = is_deep(t < 0.5 ? c0 : c1);
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

            if (best_type == bg) {
                water_alpha = is_water(bg);
                any_deep = is_deep(bg);
            } else {
                float t = 1.0 - smoothstep(-0.05, 0.05, best_sdf);
                water_alpha = mix(is_water(bg), is_water(best_type), t);
                any_deep = is_deep(t < 0.5 ? bg : best_type);
            }
        }
    }

    // Shore strip: shrink water inward so dry riverbed is visible between
    // the terrain blend edge and the water surface edge.
    float shore = 0.3;
    water_alpha = smoothstep(shore, shore + 0.15, water_alpha);

    if (water_alpha < 0.01) discard;

    // Pick color based on water type (opacity no longer used — waves drive visibility)
    vec3 tint_color = any_deep ? pc.deep_color.rgb : pc.shallow_color.rgb;

    // ── Water normal mapping: world-space, two layers at non-repeating scales ──
    vec2 uv1 = frag_world_pos.xy * 0.0008 + pc.time * vec2(0.012, 0.007);
    vec2 uv2 = frag_world_pos.xy * 0.0013 + pc.time * vec2(-0.009, 0.011);

    vec3 n1 = texture(water_normal_map, uv1).rgb * 2.0 - 1.0;
    vec3 n2 = texture(water_normal_map, uv2).rgb * 2.0 - 1.0;

    vec3 water_n = normalize(vec3(n1.xy + n2.xy, n1.z + n2.z));

    // ── Specular highlights ─────────────────────────────────────────────
    vec3 light_dir = normalize(vec3(0.3, -0.5, 0.8));
    vec3 view_dir = normalize(vec3(0.0, -0.6, 1.0));
    vec3 surface_normal = normalize(vec3(water_n.xy * 0.4, 1.0));
    vec3 half_vec = normalize(light_dir + view_dir);
    float spec = pow(max(dot(surface_normal, half_vec), 0.0), 48.0);

    // Wave crest tint
    float wave_intensity = max(water_n.x + water_n.y, 0.0) * 0.3;

    // ── Spindrift: foam at shoreline driven by wave crests ─────────────
    float foam = 0.0;
    float edge_band = smoothstep(0.0, 0.4, water_alpha) * (1.0 - smoothstep(0.6, 1.0, water_alpha));
    if (edge_band > 0.01) {
        // Wave crest from both normal layers — foam where waves peak
        float crest = max(n1.x + n1.y, 0.0) * max(n2.x + n2.y, 0.0);
        foam = edge_band * smoothstep(0.02, 0.12, crest);
    }

    // Fog
    float fog = 1.0;
    if (pc.fog_enabled > 0.5) {
        fog = texture(fog_tex, frag_fog_uv).r;
    }

    vec3 final_color;
    float final_alpha;

    if (any_deep) {
        // Deep water: opaque, dark base + wave variation + specular
        final_color = tint_color + vec3(wave_intensity * 0.1) + vec3(spec * 0.4);
        final_alpha = water_alpha;
    } else {
        // Shallow water: crystal clear, only waves and specular visible
        final_color = mix(tint_color, vec3(1.0), spec / max(spec + wave_intensity, 0.01));
        final_alpha = (spec * 0.5 + wave_intensity * 0.15) * water_alpha;

        // Spindrift foam at shoreline
        final_color = mix(final_color, vec3(0.9, 0.92, 0.88), foam);
        final_alpha = clamp(final_alpha + foam * 0.7, 0.0, 1.0);
    }

    out_color = vec4(final_color * fog, final_alpha * fog);
}
