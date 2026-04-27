// Overlay decal source generator. Emits PNG files (default:
// `build/overlay_sources/`) which `scripts/png_to_ktx2.ps1 -Linear`
// converts to KTX2 placed at `engine/textures/overlays/*.ktx2` —
// those KTX2s are the runtime assets the engine actually loads. PNGs
// are intermediates: not packed, not committed.
//
// This .cpp file is the authoritative source-of-truth; tweak the
// pixel generators here and re-run the regenerate step. An artist
// who wants to replace a decal entirely can author a PNG by hand and
// drop it through png_to_ktx2.ps1 to land at the same engine path.
//
// Decal naming reflects the WorldOverlays::TextureId slots — keeps
// the lookup table in world_overlays.cpp trivially mappable. All
// images are alpha-mask data: every pixel is (a, a, a, a) so the
// fragment shader's `texture * vertex_color` multiply produces
// premultiplied output regardless of the vertex color's RGB.
//
// Build target: `uldum_gen_overlays`. Run from the project root so
// the relative output path resolves.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr float kPi      = 3.14159265358979323846f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float smoothstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

uint8_t to_u8(float v) {
    int n = static_cast<int>(v * 255.0f + 0.5f);
    if (n < 0)   n = 0;
    if (n > 255) n = 255;
    return static_cast<uint8_t>(n);
}

// Helpers that turn an alpha mask `a` into a (a, a, a, a) RGBA pixel.
void put(std::vector<uint8_t>& px, int w, int x, int y, float a) {
    size_t i = (static_cast<size_t>(y) * w + x) * 4;
    uint8_t v = to_u8(a);
    px[i+0] = v;
    px[i+1] = v;
    px[i+2] = v;
    px[i+3] = v;
}

bool write_png(const fs::path& out, const std::vector<uint8_t>& px,
               int w, int h) {
    fs::create_directories(out.parent_path());
    auto s = out.generic_string();
    int ok = stbi_write_png(s.c_str(), w, h, 4, px.data(), w * 4);
    if (!ok) {
        std::fprintf(stderr, "  failed to write %s\n", s.c_str());
        return false;
    }
    std::printf("  wrote %s  (%dx%d)\n", s.c_str(), w, h);
    return true;
}

// ── Quad decals (256² alpha mask) ─────────────────────────────────────────

// AoE circle — magic-circle diagram with real interior pattern, no
// solid fill. From outside in:
//   • outer hairline at r=0.95
//   • mid ring at r=0.60 (slightly lighter)
//   • inner ring at r=0.22 (lighter still)
//   • 8 radial spokes (cardinal + diagonal) from r=0.22 → r=0.95
//   • center dot
//
// You can see through the AoE to whatever's underneath while the
// pattern makes the boundary, center, and orientation unambiguous.
std::vector<uint8_t> gen_aoe_circle(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4, 0);
    float half = size * 0.5f;
    float r_max = half - 1.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (x + 0.5f) - half;
            float dy = (y + 0.5f) - half;
            float r  = std::sqrt(dx*dx + dy*dy) / r_max;       // 0..1+
            float ang = std::atan2(dy, dx);                     // -π..π

            // Outer hairline ring.
            float outer = 1.0f - std::min(std::fabs(r - 0.95f) / 0.018f, 1.0f);
            outer = outer * outer;

            // Mid ring (slightly lighter, slightly narrower).
            float mid_ring = 1.0f - std::min(std::fabs(r - 0.60f) / 0.014f, 1.0f);
            mid_ring = mid_ring * mid_ring * 0.80f;

            // Inner small ring.
            float inner_ring = 1.0f - std::min(std::fabs(r - 0.22f) / 0.012f, 1.0f);
            inner_ring = inner_ring * inner_ring * 0.65f;

            // 8 radial spokes (every 45°) running between the inner
            // ring and the outer ring. Width in angle space is fixed,
            // so they get visually thinner near the rim and thicker
            // near the center — reads as "rays".
            float spokes = 0.0f;
            if (r > 0.22f && r < 0.95f) {
                constexpr float kStep = kPi / 4.0f;              // 45°
                float a_norm = ang;
                if (a_norm < 0) a_norm += 2.0f * kPi;
                float a_off = std::fmod(a_norm + kStep * 0.5f, kStep) - kStep * 0.5f;
                float ang_dist = std::fabs(a_off);
                if (ang_dist < 0.026f) {                         // ~1.5° each side
                    float t = (1.0f - ang_dist / 0.026f);
                    // Soften where the spokes cross each ring so the
                    // junction reads cleanly.
                    spokes = t * 0.55f;
                }
            }

            // Center dot — a soft small disc, alpha falls off over
            // 4 px so the marker is visible but not blocky.
            float center_dist = std::sqrt(dx*dx + dy*dy);
            float dot = 1.0f - smoothstep(2.0f, 4.0f, center_dist);
            dot *= 0.85f;

            // Hard cutoff just past the outer ring.
            float cutoff = 1.0f - smoothstep(0.95f, 1.00f, r);

            float a = std::max({outer, mid_ring, inner_ring, spokes, dot}) * cutoff;
            put(px, size, x, y, a);
        }
    }
    return px;
}

// Reticle donut — clean ring, alpha=0 at center, peak at rim, fade
// just past the rim. The previous runtime version was a quadratic
// gradient; this is a sharper-edged ring that reads as "target spot".
std::vector<uint8_t> gen_reticle_donut(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4, 0);
    float half = size * 0.5f;
    float r_max = half - 1.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (x + 0.5f) - half;
            float dy = (y + 0.5f) - half;
            float r  = std::sqrt(dx*dx + dy*dy) / r_max;

            // Main ring — peak around r=0.85, soft falloff inward
            // and outward.
            float ring = smoothstep(0.55f, 0.85f, r) * (1.0f - smoothstep(0.85f, 1.00f, r));

            // Tiny center dot at r<0.05 for "this is the point"
            // emphasis.
            float dot = 1.0f - smoothstep(0.00f, 0.05f, r);
            dot *= 0.60f;

            float a = std::max(ring, dot);
            put(px, size, x, y, a);
        }
    }
    return px;
}

// AoE cone wedge — full-width band at the rim, narrow apex, soft
// angular falloff at the edges. The geometry is a fan with V going
// from apex (V=0) to rim (V=1) and U from 0 to 1 across the angle;
// the texture painted here matches that mapping.
std::vector<uint8_t> gen_aoe_cone(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        float v = (y + 0.5f) / static_cast<float>(size);
        // Radial: hairline at v=0.96, soft inner glow.
        float rim   = 1.0f - std::min(std::fabs(v - 0.96f) / 0.022f, 1.0f);
        rim = rim * rim;
        float fill  = 0.10f * smoothstep(0.05f, 0.96f, v);
        float cutoff_v = 1.0f - smoothstep(0.96f, 1.00f, v);
        for (int x = 0; x < size; ++x) {
            float u = (x + 0.5f) / static_cast<float>(size);
            // Angular: soft falloff at u=0/1, plus thin "ray" lines
            // at u=0.04 / u=0.96 to mark the wedge boundaries.
            float u_dist = std::fabs(u - 0.5f) * 2.0f;            // 0 at center, 1 at edges
            float side_falloff = 1.0f - smoothstep(0.85f, 1.00f, u_dist);
            float ray = 1.0f - std::min(std::fabs(u_dist - 0.92f) / 0.020f, 1.0f);
            ray = ray * ray;

            float ring_total = std::max(rim, ray) * side_falloff;
            float a = std::max(ring_total, fill * side_falloff) * cutoff_v;
            put(px, size, x, y, a);
        }
    }
    return px;
}

// ── Ribbon decals (small) ─────────────────────────────────────────────────

// Ring stroke — used by selection rings, range ring, target-unit
// ring. Triangular alpha across U with a tiny solid core for crispness.
std::vector<uint8_t> gen_ring_stroke(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;            // 0 center, 1 edge
            // Soft gaussian-ish profile, sharper than a triangle.
            float a = std::pow(1.0f - u_dist, 1.5f);
            if (a < 0.0f) a = 0.0f;
            put(px, w, x, y, a);
        }
    }
    return px;
}

// Line stroke — used by AoE line. Slightly broader than ring stroke
// so a long beam reads as a full wedge rather than a thin curve.
std::vector<uint8_t> gen_line_stroke(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;
            // Plateau in the middle, soft falloff at edges.
            float a = 1.0f - smoothstep(0.55f, 1.00f, u_dist);
            put(px, w, x, y, a);
        }
    }
    return px;
}

// Curve stroke — same U profile as ring stroke, V-modulated alpha
// (t² so caster-end fades to ~0). Used by the cast arrow.
std::vector<uint8_t> gen_curve_stroke(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        float v = (y + 0.5f) / static_cast<float>(h);
        float v_alpha = v * v;
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / static_cast<float>(w);
            float u_dist = std::fabs(u - 0.5f) * 2.0f;
            float u_alpha = std::pow(1.0f - u_dist, 1.5f);
            if (u_alpha < 0.0f) u_alpha = 0.0f;
            put(px, w, x, y, u_alpha * v_alpha);
        }
    }
    return px;
}

} // namespace

int main(int argc, char** argv) {
    fs::path out_dir = "build/overlay_sources";
    if (argc > 1) out_dir = argv[1];
    std::printf("Generating overlay decals into %s\n", out_dir.generic_string().c_str());

    bool ok = true;
    {
        auto px = gen_aoe_circle(256);
        ok &= write_png(out_dir / "aoe_circle.png",   px, 256, 256);
    }
    {
        auto px = gen_reticle_donut(256);
        ok &= write_png(out_dir / "reticle.png",      px, 256, 256);
    }
    {
        auto px = gen_aoe_cone(256);
        ok &= write_png(out_dir / "aoe_cone.png",     px, 256, 256);
    }
    {
        auto px = gen_ring_stroke(64, 4);
        ok &= write_png(out_dir / "ring_stroke.png",  px, 64, 4);
    }
    {
        auto px = gen_line_stroke(64, 4);
        ok &= write_png(out_dir / "aoe_line.png",     px, 64, 4);
    }
    {
        auto px = gen_curve_stroke(64, 256);
        ok &= write_png(out_dir / "curve_stroke.png", px, 64, 256);
    }
    return ok ? 0 : 1;
}
