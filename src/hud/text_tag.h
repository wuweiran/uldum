#pragma once

// Text tags — WC3-style floating / persistent text at a world position or
// attached to a unit, with optional screen-pixel velocity and fadepoint.
// Engine-side API lives on Hud (create_text_tag / destroy / setters).
// Lua bindings land in 16c-v and forward to this same C++ surface.

#include "core/types.h"
#include "hud/hud.h"
#include "simulation/handle_types.h"

#include <glm/vec3.hpp>
#include <string>

namespace uldum::hud {

// Opaque handle. Index + generation so stale handles reliably return
// "not found" after their tag has been destroyed and its slot reused.
struct TextTagId {
    u32 index      = 0;
    u32 generation = 0;
    bool valid() const { return generation != 0; }
};

// Single-call construction — matches the Lua-side `CreateTextTag{...}`
// idiom. Either `pos` (world point) or `unit` (attached) should be set;
// if both are set, unit attachment wins.
struct TextTagCreateInfo {
    std::string        text;
    f32                px_size   = 14.0f;
    glm::vec3          pos       {0.0f};            // world point (used if unit is invalid)
    simulation::Unit   unit      {};                // attach to a unit; invalid → use pos
    f32                z_offset  = 0.0f;            // world-up height above anchor
    Color              color     = rgba(255, 255, 255, 255);
    f32                velocity_x = 0.0f;           // screen px/sec
    f32                velocity_y = 0.0f;
    f32                lifespan   = 0.0f;           // 0 → permanent
    f32                fadepoint  = 0.0f;           // seconds before end of lifespan to start fade

    // MP ownership (UINT32_MAX = broadcast — every client sees the tag).
    // Server filters sync messages by owner; each side's Hud also skips
    // rendering tags whose owner doesn't match its local player.
    u32                owner_player = UINT32_MAX;
};

} // namespace uldum::hud
