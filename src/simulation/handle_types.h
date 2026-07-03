#pragma once

#include "core/types.h"

#include <algorithm>
#include <string>
#include <vector>

namespace uldum::simulation {

// Base handle — shared layout for all game object types.
// id indexes into component storage, generation detects stale references.
struct Handle {
    u32 id         = UINT32_MAX;
    u32 generation = 0;

    bool is_valid() const { return id != UINT32_MAX; }
    bool operator==(const Handle&) const = default;
};

// Typed handles — distinct types inheriting Handle's layout.
struct Unit         : Handle {};
struct Destructable : Handle {};
struct Item         : Handle {};
struct Doodad       : Handle {};

struct Player {
    u32 id = UINT32_MAX;
    bool is_valid() const { return id != UINT32_MAX; }
    bool operator==(const Player&) const = default;
};

// Entity categories — engine-defined, fixed set.
enum class Category : u8 { Unit, Destructable, Item, Doodad, Projectile };

// Movement types — engine-defined preset because pathfinding needs them.
enum class MoveType : u8 { Ground, Air, Amphibious, Water };

// Parse MoveType from string. Returns Ground for unrecognized values.
inline MoveType parse_move_type(const std::string& s) {
    if (s == "air")        return MoveType::Air;
    if (s == "amphibious") return MoveType::Amphibious;
    if (s == "water")      return MoveType::Water;
    return MoveType::Ground;
}

// Attack target mask — which movement layers an attack can hit. One bit per
// MoveType. Used by combat target acquisition + manual attack validation so a
// ground melee unit can't hit a flyer, etc.
inline u8 move_type_bit(MoveType t) { return static_cast<u8>(1u << static_cast<u8>(t)); }

// Surface layers (Ground + Amphibious + Water) — the default when a unit's
// combat block omits "targets". Excludes Air: most units can't hit flyers
// unless they explicitly opt in.
inline constexpr u8 TARGET_MASK_SURFACE =
    (1u << static_cast<u8>(MoveType::Ground)) |
    (1u << static_cast<u8>(MoveType::Amphibious)) |
    (1u << static_cast<u8>(MoveType::Water));
inline constexpr u8 TARGET_MASK_ALL = 0x0F;  // all four MoveType layers

// Widget-target bits — the second axis of the WC3-style attack handshake.
// MoveType layers occupy bits 0-3; destructables aren't on a movement layer,
// so they present a widget classification in the high nibble instead. An attack
// can hit a destructable only if its target_mask carries the matching bit.
// DEBRIS (crates, barrels) and STRUCTURE (gates, building-like) are both in the
// default set — near-universally attackable. TREE is deliberately NOT default,
// so ordinary units can't chop trees — only attacks that opt in via
// combat.targets "tree" (siege, harvest, etc.). Mirrors WC3's "Targeted As".
inline constexpr u8 TARGET_BIT_STRUCTURE = 1u << 4;
inline constexpr u8 TARGET_BIT_TREE      = 1u << 5;
inline constexpr u8 TARGET_BIT_DEBRIS    = 1u << 6;

// Widget bits that ordinary surface attacks hit without opting in. Crates and
// barrels (debris) and building-like destructables (structure) are smashable by
// default; tree is excluded (must be named explicitly).
inline constexpr u8 TARGET_MASK_DEFAULT_WIDGETS = TARGET_BIT_STRUCTURE | TARGET_BIT_DEBRIS;

// Derive a destructable's widget target bit from its classification flags —
// the same map-defined string tags units use ("structure", "hero", …). "tree"
// is choppable only by tree-targeting attacks; "structure" reads as a building;
// anything else (incl. omitted) defaults to DEBRIS (crate/barrel: smashable).
inline u8 widget_target_from_classifications(const std::vector<std::string>& flags) {
    for (const auto& f : flags) {
        if (f == "tree")      return TARGET_BIT_TREE;
        if (f == "structure") return TARGET_BIT_STRUCTURE;
    }
    return TARGET_BIT_DEBRIS;
}

// Build a target mask from a JSON "targets" string array. Empty → surface.
// Any surface-hitting attack implicitly includes the default widget bits (so
// crates/barrels stay attackable without every unit listing them); "tree" must
// be named explicitly.
inline u8 parse_target_mask(const std::vector<std::string>& targets) {
    if (targets.empty()) return TARGET_MASK_SURFACE | TARGET_MASK_DEFAULT_WIDGETS;
    u8 mask = 0;
    for (const auto& s : targets) {
        if (s == "structure")   mask |= TARGET_BIT_STRUCTURE;
        else if (s == "tree")   mask |= TARGET_BIT_TREE;
        else if (s == "debris") mask |= TARGET_BIT_DEBRIS;
        else                    mask |= move_type_bit(parse_move_type(s));
    }
    // Implicit default widgets for any attack that can hit a surface layer.
    if (mask & TARGET_MASK_SURFACE) mask |= TARGET_MASK_DEFAULT_WIDGETS;
    return mask;
}

// Classifications are map-defined string flags (e.g., "ground", "air", "hero", "structure").
// The engine provides the infrastructure for targeting filters; maps define the actual values.
inline bool has_classification(const std::vector<std::string>& flags, const std::string& flag) {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

// Attack types, armor types, and attributes are all map-defined strings.
// The engine stores them as std::string — no enums.

} // namespace uldum::simulation
