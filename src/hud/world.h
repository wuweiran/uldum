#pragma once

// 16c-iii world UI — elements anchored to world positions / entities but
// rendered as screen-pixel-sized 2D quads on top of the 3D scene. Reuses
// the HUD quad batcher + MSDF font pipeline; the only difference from
// screen-space nodes is how positions are computed (world → projected →
// pixel, each frame).

#include "core/types.h"
#include "hud/hud.h"
#include "simulation/handle_types.h"  // Player

#include <string>
#include <vector>

namespace uldum::simulation { struct World; class FogOfWar; class TypeRegistry; }
namespace uldum::render     { class Camera; }
namespace uldum::input      { class Picker; class SelectionState; }
namespace uldum::map        { struct TerrainData; }

namespace uldum::hud {

// When is a world-anchored bar visible? Multiple policies are OR-combined
// — show if ANY condition is true.
enum class VisibilityPolicy : u8 {
    Always,
    NotFull,    // state.current < state.max
    Hovered,    // cursor is over the owning unit
    Selected,   // unit is in local player's selection
};

struct WorldBarStyle {
    Color bg_color   = rgba(0, 0, 0, 128);
    Color fill_color = rgba(220, 220, 220, 255);
};

struct WorldBarTemplate {
    std::string                    state_id;    // "health" or a custom state name
    std::vector<VisibilityPolicy>  visibility;  // OR of policies; empty → Always
    WorldBarStyle                  style;
};

struct EntityBarsConfig {
    f32  z_offset = 2.0f;   // world units above unit pivot (where the stack's top starts)
    u32  width    = 30;     // pixels per bar
    u32  height   = 3;      // pixels per bar
    u32  spacing  = 1;      // pixels between stacked bars
    std::vector<WorldBarTemplate> bars;
};

struct NameLabelStyle {
    Color color    = rgba(255, 255, 255, 255);
    Color bg_color = rgba(0, 0, 0, 160);
    bool  has_bg   = true;     // draw a background pill behind text; otherwise text-only
    f32   bg_pad_x = 6.0f;
    f32   bg_pad_y = 2.0f;
};

struct NameLabelConfig {
    f32            z_offset = 170.0f;   // world units above unit pivot
    f32            px_size  = 14.0f;
    NameLabelStyle style;
};

struct WorldOverlayConfig {
    bool             entity_bars_enabled = false;
    EntityBarsConfig entity_bars;
    bool             name_label_enabled  = false;
    NameLabelConfig  name_label;
};

// Sim-side references the HUD needs to walk entities + project + filter by
// fog. Set once at session start, cleared at session end.
struct WorldContext {
    const simulation::World*        world     = nullptr;   // authoritative for host/offline, client mirror for client
    const simulation::FogOfWar*     fog       = nullptr;   // local-player fog (client or server-side)
    const simulation::TypeRegistry* types     = nullptr;   // for resolving `type_id` → display_name
    const render::Camera*           camera    = nullptr;
    const input::Picker*            picker    = nullptr;
    const input::SelectionState*    selection = nullptr;
    const map::TerrainData*         terrain   = nullptr;
    simulation::Player              local_player{0};
};

} // namespace uldum::hud
