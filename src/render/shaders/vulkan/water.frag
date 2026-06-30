#version 450

// Water surface shading. Surface sits at terrain_z + 2.0 for all water tiles
// (matching offset in water.vert); waves are normal-only, no displacement.
// Shallow = puddle over the visible riverbed; deep = opaque sea.

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
    uint  water_mask;      // bitmask: layer IDs that are water
    uint  deep_mask;       // bitmask: deep water layers (rendered opaque)
    vec4  camera_pos;      // xyz = camera world position (w unused)
} pc;

// Per-layer water tint, indexed by tileset layer id (binding 5).
layout(set = 0, binding = 5, std140) uniform WaterColors {
    vec4 tint[16];
} water_colors;

layout(location = 1) in vec2 frag_texcoord;
layout(location = 2) centroid in vec2 frag_tile_uv;
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
    vec3  tint_color;

    if (c0 == c1 && c1 == c2 && c2 == c3) {
        // Uniform tile
        water_alpha = 1.0;
        deep_blend = d0;
        tint_color = water_colors.tint[c0].rgb;
    } else {
        vec2 uv = fract(frag_tile_uv);
        uint ct[4] = uint[4](c0, c1, c2, c3);

        // Edge-vanishing mask: keeps SDF curves anchored to midpoints
        // at shared tile edges. See matching block in terrain.frag.
        float edge_mask = 16.0 * uv.x * (1.0 - uv.x) * uv.y * (1.0 - uv.y);

        // When deep water touches land: match terrain noise exactly.
        // When only shallow water touches land: puddle-like edges.
        // When only water types meet (no land): match terrain noise.
        float noise, perturb;
        if (has_deep || !has_land) {
            noise = texture(transition_noise, frag_tile_uv).r;
            perturb = (noise - 0.5) * 0.22 * edge_mask;  // identical to terrain.frag
        } else {
            noise = texture(transition_noise, frag_tile_uv * 1.4 + vec2(0.37, 0.71)).r;
            perturb = (noise - 0.5) * 0.28 * edge_mask;
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
            tint_color = mix(water_colors.tint[c0].rgb, water_colors.tint[c2].rgb, t);
        } else if (is_edge_v) {
            float curve = (1.0 - cos(uv.y * edge_freq)) * 0.5;
            float wave = 0.5 + curve * edge_amp + perturb;
            float t = smoothstep(-edge_smooth, edge_smooth, uv.x - wave);
            water_alpha = mix(w0, w1, t);
            deep_blend = mix(d0, d1, t);
            tint_color = mix(water_colors.tint[c0].rgb, water_colors.tint[c1].rgb, t);
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
                tint_color = water_colors.tint[bg].rgb;
            } else {
                float sdf_smooth = (has_deep || !has_land) ? 0.06 : 0.05;
                float t = 1.0 - smoothstep(-sdf_smooth, sdf_smooth, best_sdf);
                water_alpha = mix(is_water(bg), is_water(best_type), t);
                deep_blend = mix(bg_deep, fg_deep, t);
                tint_color = mix(water_colors.tint[bg].rgb, water_colors.tint[best_type].rgb, t);
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

    // ── Surface color tint (per-layer, blended above) ───────────────────

    vec2 wpos = frag_world_pos.xy;
    float wt  = pc.time;

    // ── Surface normal — two models, blended by deep_blend ──────────────
    // SHALLOW: world-anchored scrolling normal map (ripples stay put over the
    //   bed). DEEP: sum of travelling sine waves (crests propagate, nothing
    //   anchored) — a scrolling texture would slide a rigid pattern and form
    //   standing "whirlpool" nodes.
    float wt2 = wt;

    // Shallow scrolling-texture normal.
    float s_scale = 0.0011;
    vec2 warp = (texture(water_normal_map, wpos * s_scale * 0.25
                         + wt2 * vec2(0.0008, 0.0011)).xy * 2.0 - 1.0) * 0.10;
    vec2 uv1 = wpos * s_scale       + wt2 * vec2( 0.006,  0.0035) + warp;
    vec2 uv2 = wpos * s_scale * 1.7 + wt2 * vec2(-0.0045, 0.0055) + warp * 0.7;
    vec3 n1 = texture(water_normal_map, uv1).rgb * 2.0 - 1.0;
    vec3 n2 = texture(water_normal_map, uv2).rgb * 2.0 - 1.0;
    vec3 shallow_n = normalize(vec3((n1.xy + n2.xy) * 0.55, n1.z + n2.z));

    // Deep travelling-wave normal: a stack of ridged SWELLS (height = -abs(sin)
    // → sharp crests, broad troughs) rolling in a narrow cone around one wind
    // direction. The big swell (octave 0) runs straight downwind; finer octaves
    // fan out, the way a real wind sea organizes (swells track the wind). All
    // octaves stay in swell range — no fine chop. Non-harmonic frequencies + a
    // low-freq domain warp keep it from repeating; flat-ish amplitude falloff so
    // no single swell dominates.
    vec3 deep_n = shallow_n;
    float sea_crest = 0.0;   // 0..1 crest height, carried to the foam stage
    if (deep_blend > 0.001) {
        const float WAVE_STEEPNESS = 10.0;
        const float WIND   = 2.1;   // wind direction (radians)
        const float SPREAD = 0.7;   // directional cone half-angle (radians)
        vec2 dwarp = (texture(water_normal_map, wpos * 0.00035
                              + wt2 * vec2(0.0015, -0.0011)).xy * 2.0 - 1.0) * 60.0;
        vec2 p = wpos + dwarp;
        vec2 grad = vec2(0.0);
        float h = 0.0, hmax = 0.0;
        float freq = 0.012, amp = 1.0, spd = 0.45;
        for (int i = 0; i < 4; i++) {
            // Direction: big swell downwind, finer octaves fan alternately out.
            float frac = float(i) * 0.333;                 // 0 .. 1 over 4 octaves
            float side = (i % 2 == 0) ? 1.0 : -1.0;
            float a = WIND + side * SPREAD * frac;
            vec2  D = vec2(cos(a), sin(a));
            float ph = dot(D, p) * freq + wt2 * spd;
            float s  = sin(ph);
            grad += D * (-sign(s) * cos(ph)) * freq * amp;   // d(-abs(sin))/dph
            h    += -abs(s) * amp;
            hmax += amp;
            freq *= 1.47;   // non-harmonic; finest octave stays a mid-swell, not chop
            amp  *= 0.66;
            spd  *= 1.18;
        }
        deep_n = normalize(vec3(-grad * WAVE_STEEPNESS, 1.0));
        sea_crest = (h / hmax + 1.0) * deep_blend;   // 0 trough .. 1 crest
    }

    vec3 water_n = normalize(mix(shallow_n, deep_n, deep_blend));

    // ── Lighting ─────────────────────────────────────────────────────────
    vec3 N = water_n;
    vec3 light_dir = normalize(env.sun_direction.xyz);
    vec3 view_dir  = normalize(pc.camera_pos.xyz - frag_world_pos);

    // Fresnel-Schlick: overhead shows tint/bed, grazing & steep facets show sky.
    float ndotv  = max(dot(N, view_dir), 0.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - ndotv, 5.0);

    vec3 reflected   = reflect(-view_dir, N);
    vec3 env_reflect = texture(env_cubemap, reflected).rgb;

    vec3 half_vec = normalize(light_dir + view_dir);

    // Sun glint. Higher shininess = tighter, sparser highlights.
    float shininess = mix(220.0, 175.0, deep_blend);
    float spec      = pow(max(dot(N, half_vec), 0.0), shininess);

    // Large-scale brightness variation + deep troughs (deep only).
    float shade1 = texture(transition_noise, wpos * 0.001  + wt * vec2( 0.004, 0.003)).r;
    float shade2 = texture(transition_noise, wpos * 0.0015 + wt * vec2(-0.003, 0.005)).r;
    float shade  = (shade1 + shade2) - 1.0;
    vec3  base   = tint_color * (1.0 + shade * 0.30 * deep_blend);

    vec3 lit_color = mix(base, env_reflect, fresnel) + vec3(spec * 0.35);

    // ── Fog ──────────────────────────────────────────────────────────────
    float fog = 1.0;
    if (pc.fog_enabled > 0.5) {
        fog = texture(fog_tex, frag_fog_uv).r;
    }

    // ── Opacity ──────────────────────────────────────────────────────────
    // Shallow ≈ clear (riverbed shows through); deep ramps to opaque. Fresnel
    // and the glint lift opacity where the surface reflects sky.
    float shallow_base = 0.10;
    float body_alpha = mix(shallow_base, 1.0, deep_blend);
    body_alpha = max(body_alpha, fresnel);
    float final_alpha = clamp(max(body_alpha, spec) * water_alpha, 0.0, 1.0);

    // ── Shore foam (reflux wash) ─────────────────────────────────────────
    // Rides the SDF shoreline (water_alpha). A slow cos pulse washes the band
    // in (bright, wide) and out (faint, thin) so the shore breathes like
    // backwash; the noise tap only froths it, never positions it.
    float shore_coverage = (1.0 - smoothstep(0.0, 0.5, water_alpha)) * water_alpha * 2.0;
    float foam_shimmer = texture(transition_noise, wpos * 0.005 + wt * vec2(0.04, 0.03)).r;
    float wash = 0.5 + 0.5 * cos(wt * 1.6 + wpos.x * 0.004 + wpos.y * 0.004); // in/out pulse
    float foam = clamp(shore_coverage * (0.5 + wash * 1.1) * (0.6 + foam_shimmer * 0.6), 0.0, 1.0);
    vec3  final_color = mix(lit_color, vec3(1.0), foam);
    final_alpha = clamp(final_alpha + foam * 0.7, 0.0, 1.0);

    // ── Open-sea whitecaps (emitted from wave crests) ────────────────────
    // Foam rides sea_crest, so it sits on the crests and travels with them; the
    // noise tap only breaks the crest line into froth, it doesn't position it.
    const bool WHITECAP_ENABLE = true;
    if (WHITECAP_ENABLE && sea_crest > 0.001) {
        const float WHITECAP_THRESHOLD = 0.82;   // higher = foam on fewer, taller crests
        float froth = texture(transition_noise, wpos * 0.01 + wt * vec2(0.03, -0.02)).r;
        float caps  = smoothstep(WHITECAP_THRESHOLD, 0.98, sea_crest) * (0.4 + froth * 0.9);
        caps = clamp(caps, 0.0, 1.0);
        final_color = mix(final_color, vec3(1.0), caps);
        final_alpha = clamp(final_alpha + caps * 0.6, 0.0, 1.0);
    }

    out_color = vec4(final_color * fog, final_alpha * fog);
}
