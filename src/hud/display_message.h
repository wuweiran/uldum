#pragma once

// Engine composite: a queued in-game message overlay. One per map,
// singleton. Authors opt in by declaring `composites.display_message`
// in hud.json; the engine pushes one line per `DisplayMessage` call
// from Lua. Newest line sits at the bottom; oldest scrolls off the
// top when `max_lines` is exceeded; each line fades out after its
// lifespan elapses. If the map didn't declare the composite, the
// `DisplayMessage` call logs to the console instead — same graceful
// degradation as the rest of the composites.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"
#include "i18n/locale.h"

#include <chrono>
#include <string>
#include <vector>

namespace uldum::hud {

enum class DisplayMessageStyleId : u8 {
    ClassicRts = 0,
};

struct DisplayMessageStyle {
    // Solid panel under the lines (transparent by default — most maps
    // want the text to float on top of the world).
    Color bg               = rgba(0,   0,   0,   0);
    Color text_color       = rgba(255, 255, 255, 255);
    f32   text_size        = 14.0f;
    // Default duration when the caller passes 0 / negative. Each line
    // is independent — adjusting this only affects new messages.
    f32   default_lifespan = 5.0f;
    // Seconds before end of lifespan that the line starts fading to 0
    // alpha. 0 = no fade (snap off at lifespan).
    f32   fadepoint        = 1.0f;
    // Maximum simultaneous lines on screen. When exceeded, the oldest
    // line is dropped immediately (no fade — the slot is needed).
    u32   max_lines        = 4;
};

struct DisplayMessageConfig {
    bool enabled = false;
    DisplayMessageStyleId style_id = DisplayMessageStyleId::ClassicRts;
    Rect      rect{};
    Placement placement{};
    DisplayMessageStyle style;
};

// Per-line runtime state. `text` is resolved against the active locale
// on push (one-shot — the active locale at render time is the one the
// player has). `age` advances by frame dt in `update_display_messages`;
// the line is removed when `age >= lifespan`.
struct DisplayMessageLine {
    i18n::LocalizedString loc;       // resolved per-frame against active locale
    f32                   age      = 0.0f;
    f32                   lifespan = 0.0f;
    f32                   fadepoint = 0.0f;
    // Bitmask of player ids this line is meant for (UINT32_MAX =
    // broadcast). The host stores the authored mask so its own draw
    // path can filter against `local_player`; clients always store
    // UINT32_MAX because they only receive the packet at all when
    // the network layer already filtered them in.
    u32                   players_mask = UINT32_MAX;
};

struct DisplayMessageRuntime {
    bool visible = true;
    std::vector<DisplayMessageLine> lines;
};

} // namespace uldum::hud
