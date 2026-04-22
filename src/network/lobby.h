#pragma once

#include "core/types.h"
#include "map/map.h"

#include <string>
#include <vector>

// LobbyState — the pre-game slot-assignment table.
//
// The map manifest declares the *topology* of slots (how many, which team,
// what color). The lobby decides *who sits in each slot* before the game
// starts. One row per manifest slot; occupant is one of:
//
//   Open     — nobody assigned. Game can't start with Open slots.
//   Computer — AI-controlled (host-only decision).
//   Human    — a connected player. `peer_id` identifies which one.
//
// In offline mode there's exactly one "peer" — the local user — with
// peer_id = LOCAL_PEER. In host/client modes peer_id matches ENet's peer id
// (exact wire scheme is finalized in 16b-iii when lobby state syncs over
// the network).

namespace uldum::network {

constexpr u32 LOCAL_PEER = 0xFFFFFFFEu;  // sentinel for "this process"

enum class SlotOccupant : u8 { Open, Computer, Human };

// One row per manifest player slot. The array index into LobbyState::slots
// IS the player id; there is no separate slot number.
//
// A Computer slot is still human-claimable — Computer is an occupancy value,
// not a lock. `locked` is a separate flag reserved for future use (e.g. a
// game distribution reserving a slot for a named NPC).
struct SlotAssignment {
    u32          team   = 0;
    std::string  color;
    SlotOccupant occupant = SlotOccupant::Open;
    bool         locked   = false;
    u32          peer_id  = 0;      // valid only when occupant == Human
    std::string  display_name;      // current label: claimer's name or base_name
    std::string  base_name;         // manifest-declared; what display_name reverts
                                    // to when the slot is released (host-side only,
                                    // not serialized — clients see display_name).
};

struct LobbyState {
    std::string                  map_path;     // e.g. "maps/test_map.uldmap"
    std::string                  map_name;     // from manifest for UI
    std::vector<SlotAssignment>  slots;        // one per manifest slot
};

// Populate `out` from a loaded map manifest. `"type": "computer"` → starts
// as Computer; anything else → starts Open. Either way the slot is
// human-claimable.
inline void init_lobby_from_manifest(LobbyState& out,
                                     std::string_view map_path,
                                     const map::MapManifest& manifest) {
    out.map_path = std::string{map_path};
    out.map_name = manifest.name;
    out.slots.clear();
    out.slots.reserve(manifest.players.size());
    for (u32 i = 0; i < manifest.players.size(); ++i) {
        const auto& p = manifest.players[i];
        SlotAssignment a;
        a.team  = p.team;
        a.color = p.color;
        if (p.type == "computer") {
            a.occupant  = SlotOccupant::Computer;
            a.base_name = p.name.empty() ? "Computer" : p.name;
        } else {
            a.occupant  = SlotOccupant::Open;
            a.base_name = p.name.empty() ? ("Player " + std::to_string(i)) : p.name;
        }
        a.display_name = a.base_name;
        out.slots.push_back(std::move(a));
    }
}

// True when every slot is Human or Computer (no Open). The game can be
// started with Open slots; this helper exists only for callers that want
// that signal (e.g. `uldum_server` auto-commits when the lobby is full).
inline bool lobby_all_slots_claimed(const LobbyState& s) {
    if (s.slots.empty()) return false;
    for (const auto& a : s.slots) {
        if (a.occupant == SlotOccupant::Open) return false;
    }
    return true;
}

// Finds the slot (array index = player id) currently claimed by `peer_id`,
// or UINT32_MAX if none.
inline u32 lobby_slot_for_peer(const LobbyState& s, u32 peer_id) {
    for (u32 i = 0; i < s.slots.size(); ++i) {
        const auto& a = s.slots[i];
        if (a.occupant == SlotOccupant::Human && a.peer_id == peer_id) return i;
    }
    return UINT32_MAX;
}

} // namespace uldum::network
