#pragma once

// Binary serialization for a scene's placement data (units,
// destructables, items, doodads, regions, cameras). Replaces the
// previous `objects.json` per-scene file — doodad-heavy maps (forests,
// dressing) can carry tens of thousands of records, where JSON's
// per-record overhead becomes the file's dominant size and load cost.
//
// Format (little-endian, no header, no version field — matches
// terrain.bin's "format-by-convention" approach):
//
//   [u32] type_id_count
//     repeated: [u8 len][bytes utf-8]            // dedupe string table; u8 len → 255-char cap
//
//   [u32] unit_count
//     repeated: [u16 type_id_index][u8 owner]
//               [f32 x][f32 y][f32 facing_deg]   // 15B
//
//   [u32] destructable_count
//     repeated: [u16 type_id_index]
//               [f32 x][f32 y][f32 facing_deg]
//               [u8 variation]                   // 15B
//
//   [u32] item_count
//     repeated: [u16 type_id_index]
//               [f32 x][f32 y]                   // 10B
//
//   [u32] doodad_count
//     repeated: [u16 type_id_index]
//               [f32 x][f32 y][f32 facing_deg]
//               [u8 variation]                   // 15B
//
//   [u32] region_count
//     repeated:
//       [u8 id_len][bytes id]                    // 255-char cap
//       [u16 rect_count]   [repeated f32 x0,y0,x1,y1]
//       [u16 circle_count] [repeated f32 cx,cy,r]
//
//   [u32] camera_count
//     repeated:
//       [u8 id_len][bytes id]                    // 255-char cap
//       [f32 target_x][f32 target_y][f32 target_z]
//       [f32 distance][f32 pitch_deg][f32 yaw_deg]
//
// All string ids (type ids, region ids, camera ids) cap at 255 bytes.
// Authors writing > 255-char ids will see a save-time warning and the
// id truncated; map-format docs surface the limit (see api.lua's
// GetRegion / GetCameraSetup comments).
//
// Facing values stored as degrees — matches the JSON authoring
// convention from objects.json. The loader converts to radians at
// runtime (same as the old JSON path).

#include "core/types.h"
#include "map/map.h"

#include <span>
#include <string_view>
#include <vector>

namespace uldum::map {

// Read a placements.bin payload into a SceneData. On parse failure,
// returns false and leaves `out` partially populated (caller should
// treat it as empty). Reuses the project's ByteReader convention —
// short reads return false at the boundary.
bool read_placements(std::span<const u8> data, SceneData& out);

// Serialize `scene` to a byte buffer. Reads the placement lists
// already populated on SceneData (units / destructables / items /
// doodads / regions / cameras). Save sites typically rebuild those
// from the live simulation world before calling this — see
// MapManager::save_objects for the pattern.
std::vector<u8> write_placements(const SceneData& scene);

} // namespace uldum::map
