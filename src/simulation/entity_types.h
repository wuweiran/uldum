#pragma once

#include "core/types.h"

#include <algorithm>
#include <string>
#include <vector>

namespace uldum::simulation {

// Entity — the stable ECS id shared by every game object. It's just the key
// that joins an object's column-split components (transform, renderable, …)
// across the World's sparse sets. This is the generic ECS identity, with no
// gameplay or scripting meaning on its own.
struct Entity {
    u32 id = UINT32_MAX;

    bool operator==(const Entity&) const = default;
};

// `Handle` is the SAME id, named for its gameplay/scripting role. In the WC3
// sense a "handle" is a script-addressable object; the typed gameplay ids
// below (Unit / Item / Destructable) are handles because they get Lua
// usertypes. Decoration (Doodad) is an Entity but NOT a handle — it has no
// script binding — so it derives straight from Entity to say so in the type.
using Handle = Entity;

inline bool is_null_entity(Entity e) {
    return e.id == UINT32_MAX;
}

inline bool is_non_null_entity(Entity e) {
    return e.id != UINT32_MAX;
}

// Gameplay-facing spelling of the same check, kept so the ~90 handle-oriented
// call sites read in handle vocabulary.
inline bool is_null_handle(Handle h) {
    return is_null_entity(h);
}

inline bool is_non_null_handle(Handle h) {
    return is_non_null_entity(h);
}

// Widget — the WC3 mid-tier: a Unit, Destructable, or Item. Common base so a
// targetable thing (order/combat target, picker return) has one type.
struct Widget : Handle {};

// Unit / Destructable / Item are Widgets; Projectile is a handle but not a widget.
struct Unit         : Widget {};
struct Destructable : Widget {};
struct Item         : Widget {};
// Projectile is a transient agent (missile / bolt): unlike WC3 — where missile
// art has no script identity — ours is script-addressable (CreateProjectile,
// GetProjectile*, projectile_hit/destroyed events), so it's a real handle.
struct Projectile   : Handle {};
// TextTag is a floating/label text object: script-addressable (CreateTextTag
// returns it, DestroyTextTag takes it) but not targetable, so it's a handle
// and not a Widget — same tier as Projectile. Its id is a shared ECS id.
struct TextTag      : Handle {};
// Doodad is pure decoration with no script binding: an Entity, not a handle.
struct Doodad       : Entity {};

struct Player {
    u32 id = UINT32_MAX;
    bool is_valid() const { return id != UINT32_MAX; }
    bool operator==(const Player&) const = default;
};

// Entity categories — engine-defined, fixed set.
enum class Category : u8 { Unit, Destructable, Item, Doodad, Projectile };

// Movement types — engine-defined preset because pathfinding needs them. This
// is the PATHING axis ONLY (how a unit traverses terrain). It does NOT drive
// attack targeting — that is the classification-based "Targeted As" axis below.
// Mirrors WC3, where Movement Type (Foot/Fly/None) and Targeted As (ground/air/
// structure) are independent fields: a flyer is hit as "air" because it's
// classified air, not because it flies.
enum class MoveType : u8 { Ground, Fly, Amphibious, Water, None };

// Parse MoveType from string. Returns Ground for unrecognized values.
inline MoveType parse_move_type(const std::string& s) {
    if (s == "fly" || s == "air") return MoveType::Fly;   // "air" kept as an alias
    if (s == "amphibious")        return MoveType::Amphibious;
    if (s == "water")             return MoveType::Water;
    if (s == "none")              return MoveType::None;
    return MoveType::Ground;
}

// Inverse of parse_move_type — the canonical lowercase name for a MoveType.
// Kept beside parse_move_type so the string↔enum mapping stays in one place.
inline const char* move_type_name(MoveType t) {
    switch (t) {
        case MoveType::Fly:        return "fly";
        case MoveType::Amphibious: return "amphibious";
        case MoveType::Water:      return "water";
        case MoveType::None:       return "none";
        case MoveType::Ground:     break;
    }
    return "ground";
}

// ── Attack targeting ("Targeted As") ──────────────────────────────────────
// The WC3-style attack handshake, fully DECOUPLED from MoveType. An attack
// carries a target_mask (which classes it may hit); a target presents its own
// class bit(s). A ground melee attack can't hit a flyer because the flyer
// presents AIR and the attack's mask lacks it — nothing to do with pathing.
//   • Units → bits from `classifications` ("air" → AIR else GROUND; "structure"
//     adds STRUCTURE). See target_class_from_classifications.
//   • Destructables → bit from `targeted_as` (structure/tree/debris). See
//     widget_target_from_targeted_as.
// TREE is deliberately NOT in the default weapon mask — ordinary units can't
// chop trees; only attacks that opt in via "targets": ["tree"] (siege/harvest).
inline constexpr u8 TARGET_BIT_GROUND    = 1u << 0;
inline constexpr u8 TARGET_BIT_AIR       = 1u << 1;
inline constexpr u8 TARGET_BIT_STRUCTURE = 1u << 2;
inline constexpr u8 TARGET_BIT_TREE      = 1u << 3;
inline constexpr u8 TARGET_BIT_DEBRIS    = 1u << 4;

// Surface (ground plane) — the default target class when a weapon omits
// "targets". Excludes AIR: units opt into anti-air explicitly.
inline constexpr u8 TARGET_MASK_SURFACE = TARGET_BIT_GROUND;

// Widget bits that ordinary surface attacks hit without opting in. Crates and
// barrels (debris) and building-like destructables (structure) are smashable by
// default; tree is excluded (must be named explicitly).
inline constexpr u8 TARGET_MASK_DEFAULT_WIDGETS = TARGET_BIT_STRUCTURE | TARGET_BIT_DEBRIS;

// Derive a destructable's widget target bit from its "targeted_as" flags —
// WC3's "Targeted As" axis (how a thing is hit). "tree" is choppable only by
// tree-targeting attacks; "structure" reads as a building; anything else
// (incl. omitted) defaults to DEBRIS (crate/barrel: smashable).
inline u8 widget_target_from_targeted_as(const std::vector<std::string>& flags) {
    for (const auto& f : flags) {
        if (f == "tree")      return TARGET_BIT_TREE;
        if (f == "structure") return TARGET_BIT_STRUCTURE;
    }
    return TARGET_BIT_DEBRIS;
}

// Build a target mask from a JSON "targets" string array. Empty → surface +
// default widgets. "ground"/"water"/"amphibious" all present as GROUND for
// targeting (WC3 targets naval as ground); only "air" opts into anti-air.
inline u8 parse_target_mask(const std::vector<std::string>& targets) {
    if (targets.empty()) return TARGET_MASK_SURFACE | TARGET_MASK_DEFAULT_WIDGETS;
    u8 mask = 0;
    for (const auto& s : targets) {
        if      (s == "air")       mask |= TARGET_BIT_AIR;
        else if (s == "structure") mask |= TARGET_BIT_STRUCTURE;
        else if (s == "tree")      mask |= TARGET_BIT_TREE;
        else if (s == "debris")    mask |= TARGET_BIT_DEBRIS;
        else                       mask |= TARGET_BIT_GROUND;   // ground/water/amphibious
    }
    // Implicit default widgets for any attack that can hit the ground plane.
    if (mask & TARGET_MASK_SURFACE) mask |= TARGET_MASK_DEFAULT_WIDGETS;
    return mask;
}

// Human-readable name of a target-class bitset — for input reject feedback
// ("your attack can't hit air"). Reports the most-specific class first.
inline const char* target_class_name(u8 bits) {
    if (bits & TARGET_BIT_AIR)       return "air";
    if (bits & TARGET_BIT_TREE)      return "tree";
    if (bits & TARGET_BIT_STRUCTURE) return "structure";
    if (bits & TARGET_BIT_DEBRIS)    return "debris";
    return "ground";
}

// Classifications are map-defined string flags (e.g., "ground", "air", "hero", "structure").
// The engine provides the infrastructure for targeting filters; maps define the actual values.
inline bool has_classification(const std::vector<std::string>& flags, const std::string& flag) {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

// Derive a unit's "Targeted As" bits from its classification flags — the unit
// counterpart of widget_target_from_targeted_as. This is what decouples
// targeting from MoveType: a flyer is AIR because it's classified "air", not
// because it flies. "air" → AIR (else GROUND); "structure" adds STRUCTURE so
// towers are hit by structure-targeting attacks. Empty/absent → GROUND.
inline u8 target_class_from_classifications(const std::vector<std::string>& flags) {
    u8 bits = has_classification(flags, "air") ? TARGET_BIT_AIR : TARGET_BIT_GROUND;
    if (has_classification(flags, "structure")) bits |= TARGET_BIT_STRUCTURE;
    return bits;
}

// Attack types, armor types, and attributes are all map-defined strings.
// The engine stores them as std::string — no enums.

} // namespace uldum::simulation
