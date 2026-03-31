#pragma once

#include "core/types.h"

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

// Classification flags (bitmask)
enum class Classification : u32 {
    None        = 0,
    Ground      = 1 << 0,
    Air         = 1 << 1,
    Mechanical  = 1 << 2,
    Undead      = 1 << 3,
    Worker      = 1 << 4,
    Ancient     = 1 << 5,
    Hero        = 1 << 6,
    Structure   = 1 << 7,
    Summoned    = 1 << 8,
};

inline Classification operator|(Classification a, Classification b) {
    return static_cast<Classification>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline Classification operator&(Classification a, Classification b) {
    return static_cast<Classification>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline bool has_flag(Classification flags, Classification test) {
    return (flags & test) == test;
}

// Enums
enum class Category : u8 { Unit, Destructable, Item, Doodad, Projectile };
enum class MoveType : u8 { Ground, Air, Amphibious };
enum class ArmorType : u8 { Unarmored, Light, Medium, Heavy, Fortified, Hero };
enum class AttackType : u8 { Normal, Pierce, Siege, Magic, Chaos, Hero };
enum class Attribute : u8 { Str, Agi, Int };

} // namespace uldum::simulation
