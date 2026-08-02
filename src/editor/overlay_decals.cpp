#include "editor/overlay_decals.h"

#include <algorithm>
#include <cmath>

namespace uldum::editor::overlay_decals {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float smoothstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
uint8_t to_u8(float v) {
    int n = static_cast<int>(v * 255.0f + 0.5f);
    return static_cast<uint8_t>(n < 0 ? 0 : (n > 255 ? 255 : n));
}

// Alpha `a` → (a,a,a,a) so the overlay shader's texture*vertex_color multiply
// yields premultiplied output regardless of the per-draw tint's RGB.
void put(std::vector<u8>& px, int w, int x, int y, float a) {
    size_t i = (static_cast<size_t>(y) * w + x) * 4;
    u8 v = to_u8(a);
    px[i+0] = v; px[i+1] = v; px[i+2] = v; px[i+3] = v;
}

Decal make(std::string name, int w, int h) {
    Decal d;
    d.filename = std::move(name);
    d.w = static_cast<u32>(w);
    d.h = static_cast<u32>(h);
    d.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
    return d;
}

// ── Original set ──────────────────────────────────────────────────────────

// Magic-circle diagram: outer hairline, mid + inner rings, 8 radial spokes,
// center dot — see-through interior, unambiguous boundary/center/orientation.
Decal gen_aoe_circle(int size) {
    Decal d = make("aoe_circle.ktx2", size, size);
    float half = size * 0.5f, r_max = half - 1.0f;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        float dx = (x + 0.5f) - half, dy = (y + 0.5f) - half;
        float r = std::sqrt(dx*dx + dy*dy) / r_max;
        float ang = std::atan2(dy, dx);
        float outer = 1.0f - std::min(std::fabs(r - 0.95f) / 0.018f, 1.0f); outer *= outer;
        float mid = 1.0f - std::min(std::fabs(r - 0.60f) / 0.014f, 1.0f); mid = mid*mid*0.80f;
        float inner = 1.0f - std::min(std::fabs(r - 0.22f) / 0.012f, 1.0f); inner = inner*inner*0.65f;
        float spokes = 0.0f;
        if (r > 0.22f && r < 0.95f) {
            constexpr float kStep = kPi / 4.0f;
            float a_norm = ang; if (a_norm < 0) a_norm += 2.0f * kPi;
            float a_off = std::fmod(a_norm + kStep * 0.5f, kStep) - kStep * 0.5f;
            float ad = std::fabs(a_off);
            if (ad < 0.026f) spokes = (1.0f - ad / 0.026f) * 0.55f;
        }
        float cd = std::sqrt(dx*dx + dy*dy);
        float dot = (1.0f - smoothstep(2.0f, 4.0f, cd)) * 0.85f;
        float cutoff = 1.0f - smoothstep(0.95f, 1.00f, r);
        put(d.pixels, size, x, y, std::max({outer, mid, inner, spokes, dot}) * cutoff);
    }
    return d;
}

// Clean ring, alpha 0 at center, peak at rim, tiny center dot.
Decal gen_reticle(int size) {
    Decal d = make("reticle.ktx2", size, size);
    float half = size * 0.5f, r_max = half - 1.0f;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        float dx = (x + 0.5f) - half, dy = (y + 0.5f) - half;
        float r = std::sqrt(dx*dx + dy*dy) / r_max;
        float ring = smoothstep(0.55f, 0.85f, r) * (1.0f - smoothstep(0.85f, 1.00f, r));
        float dot = (1.0f - smoothstep(0.00f, 0.05f, r)) * 0.60f;
        put(d.pixels, size, x, y, std::max(ring, dot));
    }
    return d;
}

// Cone wedge in fan UV: hairline rim at V=0.96, soft inner glow, thin boundary
// rays at the wedge edges.
Decal gen_aoe_cone(int size) {
    Decal d = make("aoe_cone.ktx2", size, size);
    for (int y = 0; y < size; ++y) {
        float v = (y + 0.5f) / static_cast<float>(size);
        float rim = 1.0f - std::min(std::fabs(v - 0.96f) / 0.022f, 1.0f); rim *= rim;
        float fill = 0.10f * smoothstep(0.05f, 0.96f, v);
        float cutoff_v = 1.0f - smoothstep(0.96f, 1.00f, v);
        for (int x = 0; x < size; ++x) {
            float u = (x + 0.5f) / static_cast<float>(size);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;
            float side = 1.0f - smoothstep(0.85f, 1.00f, u_dist);
            float ray = 1.0f - std::min(std::fabs(u_dist - 0.92f) / 0.020f, 1.0f); ray *= ray;
            float a = std::max(std::max(rim, ray) * side, fill * side) * cutoff_v;
            put(d.pixels, size, x, y, a);
        }
    }
    return d;
}

// Ring stroke ribbon (selection/range/target rings). U = across-width profile;
// V (around the ring) is uniform, so 64x4 is enough.
Decal gen_ring_stroke(int w, int h) {
    Decal d = make("ring_stroke.ktx2", w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float u = (x + 0.5f) / static_cast<float>(w);
        float u_dist = std::fabs(u - 0.5f) * 2.0f;
        put(d.pixels, w, x, y, std::pow(1.0f - u_dist, 1.5f));
    }
    return d;
}

// AoE line beam at 256^2 — a real 2-D pattern (not a flat stroke). U = beam
// width (soft-edged), V = along the beam, carrying chevrons that flow toward the
// impact end (V=1) so the beam reads as directional energy.
Decal gen_aoe_line(int size) {
    Decal d = make("aoe_line.ktx2", size, size);
    constexpr float kChevrons = 6.0f;   // repeats along the beam
    for (int y = 0; y < size; ++y) {
        float v = (y + 0.5f) / static_cast<float>(size);   // 0 caster → 1 impact
        for (int x = 0; x < size; ++x) {
            float u = (x + 0.5f) / static_cast<float>(size);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;      // 0 center → 1 edge
            // Soft beam body: plateau in the middle, fade at the edges.
            float body = (1.0f - smoothstep(0.55f, 1.00f, u_dist)) * 0.28f;
            // Chevron: a V-shape whose apex leads toward v=1. The band position
            // along the beam is `phase`; brightness peaks on the chevron line.
            float phase = std::fmod(v * kChevrons + u_dist * 0.5f, 1.0f);
            float chev = (1.0f - std::min(phase / 0.16f, 1.0f)); chev *= chev;
            chev *= (1.0f - smoothstep(0.80f, 1.00f, u_dist));   // fade at edges
            // Fade the whole beam up from the caster end so it points.
            float lead = smoothstep(0.0f, 0.15f, v);
            put(d.pixels, size, x, y, std::max(body, chev) * lead);
        }
    }
    return d;
}

// Cast arrow: U = ring-stroke profile, V modulates alpha (t^2) so the caster
// end fades to ~0.
Decal gen_curve_stroke(int w, int h) {
    Decal d = make("curve_stroke.ktx2", w, h);
    for (int y = 0; y < h; ++y) {
        float v = (y + 0.5f) / static_cast<float>(h);
        float v_alpha = v * v;
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;
            put(d.pixels, w, x, y, std::pow(1.0f - u_dist, 1.5f) * v_alpha);
        }
    }
    return d;
}

// Snap-target column: single bottom-rooted glow, cylindrical cross-section.
Decal gen_snap_target(int w, int h) {
    Decal d = make("snap_target.ktx2", w, h);
    for (int y = 0; y < h; ++y) {
        float v = (y + 0.5f) / static_cast<float>(h);   // 0 top → 1 base
        float v_alpha = smoothstep(0.0f, 0.85f, v) * 0.6f;   // brighter at the base
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float uc = (u - 0.5f) * 2.0f;
            float u_alpha = std::sqrt(std::max(0.0f, 1.0f - uc*uc));
            put(d.pixels, w, x, y, u_alpha * v_alpha);
        }
    }
    return d;
}

// ── Action set (visually distinct alternates) ─────────────────────────────

// MOBA-style targeting reticle: single confident rim, soft radial fill, thin
// crosshair.
Decal gen_aoe_circle_action(int size) {
    Decal d = make("aoe_circle.ktx2", size, size);
    float half = size * 0.5f, r_max = half - 1.0f;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        float dx = (x + 0.5f) - half, dy = (y + 0.5f) - half;
        float r = std::sqrt(dx*dx + dy*dy) / r_max;
        float ring = 1.0f - std::min(std::fabs(r - 0.93f) / 0.035f, 1.0f); ring *= ring;
        float fill = (1.0f - smoothstep(0.0f, 0.92f, r)) * 0.20f;
        float cd = std::sqrt(dx*dx + dy*dy);
        float ch = (std::fabs(dy) < 1.0f && cd < 10.0f) ? 0.75f : 0.0f;
        float cv = (std::fabs(dx) < 1.0f && cd < 10.0f) ? 0.75f : 0.0f;
        float dot = (1.0f - smoothstep(2.5f, 4.5f, cd)) * 0.8f;
        float cutoff = 1.0f - smoothstep(0.93f, 0.98f, r);
        put(d.pixels, size, x, y, std::max({ring, fill, std::max(ch, cv), dot}) * cutoff);
    }
    return d;
}

// Crisp techy band: narrower plateau, harder edges (vs the original gaussian).
Decal gen_ring_stroke_action(int w, int h) {
    Decal d = make("ring_stroke.ktx2", w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float u = (x + 0.5f) / static_cast<float>(w);
        float u_dist = std::fabs(u - 0.5f) * 2.0f;
        put(d.pixels, w, x, y, 1.0f - smoothstep(0.45f, 0.85f, u_dist));
    }
    return d;
}

// Laser-beam arrow: linear V-fade (steadier body) + sharper U profile.
Decal gen_curve_stroke_action(int w, int h) {
    Decal d = make("curve_stroke.ktx2", w, h);
    for (int y = 0; y < h; ++y) {
        float v = (y + 0.5f) / static_cast<float>(h);
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;
            put(d.pixels, w, x, y, std::pow(1.0f - u_dist, 2.5f) * v);
        }
    }
    return d;
}

// Pinned marker: two bright bands (top + bottom), dim middle.
Decal gen_snap_target_action(int w, int h) {
    Decal d = make("snap_target.ktx2", w, h);
    for (int y = 0; y < h; ++y) {
        float v = (y + 0.5f) / static_cast<float>(h);
        float bot = 1.0f - smoothstep(0.00f, 0.28f, v);
        float top = smoothstep(0.72f, 1.00f, v);
        float v_alpha = std::max(bot, top) * 0.55f;
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float uc = (u - 0.5f) * 2.0f;
            float u_alpha = std::sqrt(std::max(0.0f, 1.0f - uc*uc));
            put(d.pixels, w, x, y, u_alpha * v_alpha);
        }
    }
    return d;
}

} // namespace

std::vector<Decal> generate(Style style) {
    std::vector<Decal> out;
    if (style == Style::Action) {
        // Action overrides four slots; the rest reuse the original look.
        out.push_back(gen_aoe_circle_action(256));
        out.push_back(gen_ring_stroke_action(64, 4));
        out.push_back(gen_curve_stroke_action(64, 256));
        out.push_back(gen_snap_target_action(32, 256));
        out.push_back(gen_aoe_cone(256));
        out.push_back(gen_aoe_line(256));
        out.push_back(gen_reticle(256));
    } else {
        out.push_back(gen_aoe_circle(256));
        out.push_back(gen_reticle(256));
        out.push_back(gen_aoe_cone(256));
        out.push_back(gen_ring_stroke(64, 4));
        out.push_back(gen_aoe_line(256));
        out.push_back(gen_curve_stroke(64, 256));
        out.push_back(gen_snap_target(32, 256));
    }
    return out;
}

} // namespace uldum::editor::overlay_decals
