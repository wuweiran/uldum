#pragma once

#include "core/types.h"

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace uldum::hud {

class Hud;

// Parse a `hud.json` document into the HUD's node tree. Parses the `nodes`
// array (atoms: panel, label, image, bar, button) recursively, resolving
// parent-relative anchors + offsets into the absolute screen-pixel rects
// the existing node atoms expect.
//
// The `composites` block is recognized but not yet applied — v1 composites
// (action_bar, minimap, chat_box, joystick) land in a later sub-phase.
// The `preset` string is read but currently unused beyond being logged.
//
// `viewport_w`, `viewport_h` are the current window size in logical pixels;
// they become the root's rect for anchoring. Returns true if the document
// was structurally valid (individual bad nodes are logged and skipped).
//
// Call this after map content has loaded but before the first frame of
// Playing state. Existing root children are cleared first so re-entering
// a session produces a clean tree.
bool load_from_json(Hud& hud, const nlohmann::json& doc,
                    u32 viewport_w, u32 viewport_h);

// Convenience wrapper: resolves `asset_path` through the AssetManager,
// parses the resulting bytes as JSON, and forwards to `load_from_json`.
// Returns false (quietly) if the file is missing — a map without a
// `hud.json` is legal and results in an empty HUD.
bool load_from_asset(Hud& hud, std::string_view asset_path,
                     u32 viewport_w, u32 viewport_h);

// Per-instance placement. Templates define *what* a node is; Lua's
// CreateNode supplies *where* it goes via this struct. Applied only at
// the template's root — child nodes inherit their placement from the
// template JSON (they position relative to their parent's rect, which
// is internal to the template's design).
struct TemplatePlacement {
    std::string_view anchor = "tl";   // 9-point enum
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;
    // MP ownership: UINT32_MAX = broadcast (all clients). Specific slot
    // id = only that player's client sees it and gets sync messages.
    u32 owner_player = UINT32_MAX;
};

// Instantiate a node template previously registered from hud.json into
// the HUD's root with the given placement. Fails (returns false) if the
// template is unknown or a node with the same id is already alive.
// Called from Lua's CreateNode(id, { anchor, x, y, w, h }).
bool instantiate_template(Hud& hud, std::string_view template_id,
                          u32 viewport_w, u32 viewport_h,
                          const TemplatePlacement& placement);

} // namespace uldum::hud
