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

struct Player {
    u32 id = UINT32_MAX;
    bool is_valid() const { return id != UINT32_MAX; }
    bool operator==(const Player&) const = default;
};

// Entity categories — engine-defined, fixed set.
enum class Category : u8 { Unit, Destructable, Item, Doodad, Projectile };

// Movement types — engine-defined preset because pathfinding needs them.
enum class MoveType : u8 { Ground, Air, Amphibious };

// Parse MoveType from string. Returns Ground for unrecognized values.
inline MoveType parse_move_type(const std::string& s) {
    if (s == "air")        return MoveType::Air;
    if (s == "amphibious") return MoveType::Amphibious;
    return MoveType::Ground;
}

// Classifications are map-defined string flags (e.g., "ground", "air", "hero", "structure").
// The engine provides the infrastructure for targeting filters; maps define the actual values.
inline bool has_classification(const std::vector<std::string>& flags, const std::string& flag) {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

// Attack types, armor types, and attributes are all map-defined strings.
// The engine stores them as std::string — no enums.

} // namespace uldum::simulation
