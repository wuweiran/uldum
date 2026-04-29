#pragma once

// Engine composite: per-unit inventory grid. One per map, singleton.
// Slot layout + styling come from the map's hud.json
// `composites.inventory` block. Slot contents are automatic: each frame
// the bar reads the local player's selection, picks the first selected
// unit, and fills slot[i] with the item handle in inventory.slots[i].
//
// Each slot shows, at render time:
//   - the item type's icon (never an ability icon)
//   - charges badge (bottom-right) when item.charges > 0
//   - level badge (top-left) when item.level > 0
//   - cooldown radial overlay from the item's first ability when on cd
//   - hotkey badge (top-right) for the slot's positional hotkey
//   - disabled tint when on cooldown / unaffordable (active items)
//
// Click / tap on an active item slot fires the item's first ability
// (`abilities[0]`) through the cast pump with the item handle attached
// as `source_item`, so triggers reading `GetTriggerItem()` resolve to
// this item. Passive items render but never fire on click.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"

#include <string>
#include <vector>

namespace uldum::hud {

struct InventorySlotStyle {
    Color bg            = rgba(48,  52,  68,  240);
    Color hover_bg      = rgba(72,  76,  96,  250);
    Color press_bg      = rgba(102, 106, 132, 255);
    Color empty_bg      = rgba(28,  30,  40,  180);   // slot with no item
    Color border_color  = rgba(0,   0,   0,   128);
    f32   border_width  = 1.0f;
};

struct InventorySlot {
    // Absolute screen rect (resolved from slot-relative (x,y,w,h) against
    // the bar's anchor rect). `placement` keeps the raw anchor/offset
    // values so the rect can be recomputed on viewport resize.
    Rect      rect{};
    Placement placement{};
    // Hotkey letter shown in the corner. Display-only for v1 — keyboard
    // dispatch is not wired yet.
    std::string hotkey;
    InventorySlotStyle style;

    bool visible = true;

    // Transient input state.
    bool hovered = false;
    bool pressed = false;
};

enum class InventoryStyleId : u8 {
    ClassicRts = 0,
};

struct InventoryStyle {
    Color bg                  = rgba(0,   0,   0,   0);   // transparent by default

    // Cooldown radial overlay for the slot's active ability.
    Color cooldown_overlay    = rgba(0,   0,   0,   192);
    Color cooldown_text_color = rgba(255, 255, 255, 255);
    f32   cooldown_text_size  = 18.0f;

    // Hotkey badge (top-right corner).
    Color hotkey_color        = rgba(240, 240, 240, 255);
    Color hotkey_badge_bg     = rgba(0,   0,   0,   180);

    // Charges badge (bottom-right corner) — small text "3" on a pill.
    Color charges_color       = rgba(255, 235, 180, 255);
    Color charges_badge_bg    = rgba(0,   0,   0,   190);
    f32   charges_text_size   = 14.0f;

    // Level badge (top-left corner) — small text on a pill.
    Color level_color         = rgba(180, 220, 255, 255);
    Color level_badge_bg      = rgba(0,   0,   0,   190);
    f32   level_text_size     = 13.0f;

    // Dim overlay for unaffordable / on-cooldown active items.
    Color disabled_tint       = rgba(0,   0,   0,   128);
};

struct InventoryConfig {
    bool enabled = false;
    InventoryStyleId style_id = InventoryStyleId::ClassicRts;
    // Absolute screen rect for the whole bar (for optional bg draw).
    Rect      rect{};
    Placement placement{};
    InventoryStyle style;
    std::vector<InventorySlot> slots;
};

struct InventoryRuntime {
    bool visible = true;
};

} // namespace uldum::hud
