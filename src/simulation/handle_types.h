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
inline constexpr u8 TARGET_MASK_ALL = 0x0F;  // all four layers

// Build a target mask from a JSON "targets" string array. Empty → surface.
inline u8 parse_target_mask(const std::vector<std::string>& targets) {
    if (targets.empty()) return TARGET_MASK_SURFACE;
    u8 mask = 0;
    for (const auto& s : targets) mask |= move_type_bit(parse_move_type(s));
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
