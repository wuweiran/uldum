#pragma once

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"
#include "simulation/handle_types.h"

#include <vector>

namespace uldum::hud {

struct PickupBarSlotStyle {
    Color bg           = rgba(48, 52, 68, 240);
    Color hover_bg     = rgba(72, 76, 96, 250);
    Color press_bg     = rgba(102, 106, 132, 255);
    Color border_color = rgba(0, 0, 0, 128);
    f32 border_width   = 1.0f;
};

struct PickupBarSlot {
    Rect rect{};
    Placement placement{};
    PickupBarSlotStyle style;
    bool visible = true;
    bool hovered = false;
    bool pressed = false;
};

struct PickupBarStyle {
    Color bg = rgba(0, 0, 0, 0);
};

enum class PickupBarStyleId : u8 {
    Slots = 0,
    List  = 1,
};

struct PickupBarListStyle {
    u32 max_rows = 4;
    f32 row_height = 56.0f;
    f32 row_gap = 4.0f;
    f32 padding = 4.0f;
    f32 icon_size = 44.0f;
    Color row_bg = rgba(48, 52, 68, 240);
    Color row_hover_bg = rgba(72, 76, 96, 250);
    Color row_press_bg = rgba(102, 106, 132, 255);
    Color border_color = rgba(0, 0, 0, 128);
    f32 border_width = 1.0f;
    Color name_color = rgba(255, 255, 255, 255);
    f32 name_text_size = 15.0f;
    Color description_color = rgba(200, 200, 200, 255);
    f32 description_text_size = 12.0f;
};

struct PickupBarConfig {
    bool enabled = false;
    Rect rect{};
    Placement placement{};
    f32 discovery_radius = 320.0f;
    PickupBarStyleId style_id = PickupBarStyleId::Slots;
    PickupBarStyle style;
    PickupBarListStyle list_style;
    std::vector<PickupBarSlot> slots;
};

struct PickupBarEntry {
    simulation::Unit unit;
    simulation::Item item;

    bool operator==(const PickupBarEntry&) const = default;
};

struct PickupBarRuntime {
    bool visible = true;
    std::vector<PickupBarEntry> entries;
};

} // namespace uldum::hud
