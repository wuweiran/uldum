#pragma once

// Procedural overlay-decal generator. Emits the ground-plane decal masks
// WorldOverlays uses — selection ring, AoE circle/cone/line, cast arrow,
// reticle, snap-target column — as RGBA `(a,a,a,a)` alpha masks. Used by the
// New Map bootstrapper, which encodes each to KTX2 (linear, UASTC, mipmapped)
// into the new map's textures/overlays/. See docs/overlay-textures.md.
//
// Two style sets: Original (magic-circle AoE, soft rings) and Action (MOBA
// reticle, crisp techy rings, twin-band snap column) — the two looks the shipped
// maps use.

#include "core/types.h"

#include <string>
#include <vector>

namespace uldum::editor::overlay_decals {

enum class Style : u8 { Original, Action };

// One decal ready to encode. `pixels` is w*h*4 RGBA with R=G=B=A (alpha mask).
struct Decal {
    std::string     filename;   // e.g. "ring_stroke.ktx2"
    u32             w = 0;
    u32             h = 0;
    std::vector<u8> pixels;
};

// Build the full decal set for a style (plain filenames, ready to write into a
// map's textures/overlays/).
std::vector<Decal> generate(Style style);

} // namespace uldum::editor::overlay_decals
