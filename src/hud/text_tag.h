#pragma once

// Text tags — floating / persistent text at a world position or attached
// to a unit. A `style` preset drives the motion + fade; per-tag knobs
// override the preset's defaults. Engine-side API lives on Hud
// (create_text_tag / destroy / setters); Lua bindings forward to it.

#include "core/types.h"
#include "hud/hud.h"
#include "i18n/locale.h"
#include "simulation/entity_types.h"

#include <glm/vec3.hpp>
#include <string>

namespace uldum::hud {

// A text tag's handle IS its shared ECS entity id (host-allocated,
// shipped at creation, identical on every client). UINT32_MAX = invalid.
// Transient styles (rise/wander/pop) self-expire by lifespan; only
// permanent tags are destroyed by id via DestroyTextTag.

// Motion preset. Rise/Wander/Pop are one-shot (finite lifespan, evaluated
// from `age`); Permanent is stationary and never fades. Wander's sway
// phase is a per-client local random — cosmetic, not synced.
enum class TextTagStyle : u8 {
    Rise      = 0,   // straight up, fade out (classic damage number)
    Wander    = 1,   // up while swaying L<->R, fade out
    Pop       = 2,   // up while growing to `scale_end`, fade out
    Permanent = 3,   // no motion, no fade (label; destroy by id)
};

// Single-call construction — matches the Lua-side `CreateTextTag{...}`
// idiom. Either `pos` (world point) or `unit` (attached) should be set;
// if both are set, unit attachment wins.
//
// Text content is a `LocalizedString` (the L() payload from Lua). The
// network ships {key, args}; each client resolves against its own
// active locale at render time. There is no literal-string code path:
// player-facing text always flows through the locale resolver. For
// runtime-formatted content (numbers, names) pass them as args to a
// template key.
struct TextTagCreateInfo {
    simulation::TextTag id{};                          // shared ECS handle; host allocates, client gets it off the wire
    i18n::LocalizedString   text;
    TextTagStyle       style     = TextTagStyle::Rise;
    f32                     px_size   = 14.0f;
    glm::vec3          pos       {0.0f};            // world point (used if unit is invalid)
    simulation::Unit   unit      {};                // attach to a unit; invalid → use pos
    f32                z_offset  = 0.0f;            // world-up height above anchor
    Color              color     = rgba(255, 255, 255, 255);
    f32                speed      = 0.0f;           // upward screen px/sec (Rise/Wander/Pop)
    f32                spread     = 0.0f;           // Wander sway amplitude, px
    f32                scale_end  = 1.0f;           // Pop end size multiplier
    f32                lifespan   = 0.0f;           // 0 → permanent
    f32                fade       = 0.0f;           // fade-out tail, seconds before end of lifespan

    // MP target mask (bit N = player N; UINT32_MAX = broadcast). Server
    // filters sync messages against this; each side's Hud also skips
    // rendering tags whose bit isn't set for its local player.
    u32                players_mask = UINT32_MAX;
};

} // namespace uldum::hud
