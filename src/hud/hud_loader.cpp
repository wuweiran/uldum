#include "hud/hud_loader.h"

#include "hud/hud.h"
#include "hud/node.h"
#include "hud/world.h"
#include "hud/action_bar.h"
#include "hud/minimap.h"
#include "hud/command_bar.h"
#include "hud/joystick.h"
#include "hud/layout.h"
#include "asset/asset.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace uldum::hud {

namespace {

constexpr const char* TAG = "HUD.Loader";

// Parse "#RRGGBB" or "#RRGGBBAA". Any parse failure returns opaque white
// and logs once — the layout still renders so mistakes are visible, not
// silent.
Color parse_color(const nlohmann::json& j) {
    if (!j.is_string()) {
        log::warn(TAG, "color must be a string");
        return rgba(255, 255, 255, 255);
    }
    std::string s = j.get<std::string>();
    if (s.empty() || s[0] != '#') {
        log::warn(TAG, "color '{}' missing leading '#'", s);
        return rgba(255, 255, 255, 255);
    }
    auto hex_byte = [](char c1, char c2) -> u8 {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        int a = v(c1), b = v(c2);
        if (a < 0 || b < 0) return 0;
        return static_cast<u8>((a << 4) | b);
    };
    if (s.size() == 7) {  // #RRGGBB
        return rgba(hex_byte(s[1], s[2]), hex_byte(s[3], s[4]), hex_byte(s[5], s[6]), 255);
    }
    if (s.size() == 9) {  // #RRGGBBAA
        return rgba(hex_byte(s[1], s[2]), hex_byte(s[3], s[4]), hex_byte(s[5], s[6]),
                    hex_byte(s[7], s[8]));
    }
    log::warn(TAG, "color '{}' must be #RRGGBB or #RRGGBBAA", s);
    return rgba(255, 255, 255, 255);
}

// parse_anchor / resolve_rect live in hud/layout.h so the resize-time
// recompute can reuse them.

Label::Align parse_align(std::string_view s) {
    if (s == "center") return Label::Align::Center;
    if (s == "right")  return Label::Align::Right;
    return Label::Align::Left;  // default
}

Bar::Orientation parse_orientation(std::string_view s) {
    if (s == "vertical") return Bar::Orientation::Vertical;
    return Bar::Orientation::Horizontal;
}

Bar::Direction parse_direction(std::string_view s) {
    if (s == "rtl") return Bar::Direction::Rtl;
    if (s == "btt") return Bar::Direction::Btt;
    if (s == "ttb") return Bar::Direction::Ttb;
    return Bar::Direction::Ltr;
}

// Forward decl for recursion.
void parse_node(const nlohmann::json& jn, Node& parent, const Rect& parent_rect);

// Dispatch on `type`, construct the matching atom as a child of `parent`,
// fill in its fields from `jn`. Returns a pointer to the newly-attached
// node (or nullptr on unknown type).
Node* build_atom(const nlohmann::json& jn, Node& parent, const Rect& rect) {
    std::string type = jn.value("type", "panel");

    if (type == "panel") {
        auto& n = parent.add_child<Panel>();
        n.rect = rect;
        if (auto it = jn.find("style"); it != jn.end() && it->is_object()) {
            if (auto bg = it->find("bg"); bg != it->end()) n.bg = parse_color(*bg);
        }
        return &n;
    }
    if (type == "label") {
        auto& n = parent.add_child<Label>();
        n.rect = rect;
        n.text = jn.value("text", "");
        if (auto it = jn.find("style"); it != jn.end() && it->is_object()) {
            if (auto v = it->find("color"); v != it->end()) n.color = parse_color(*v);
            if (auto v = it->find("size");  v != it->end() && v->is_number()) n.px_size = v->get<f32>();
            if (auto v = it->find("align"); v != it->end() && v->is_string()) n.align = parse_align(v->get<std::string>());
        }
        return &n;
    }
    if (type == "bar") {
        auto& n = parent.add_child<Bar>();
        n.rect = rect;
        if (auto v = jn.find("fill"); v != jn.end() && v->is_number()) n.fill = v->get<f32>();
        if (auto it = jn.find("style"); it != jn.end() && it->is_object()) {
            if (auto v = it->find("bg_color");   v != it->end()) n.bg_color   = parse_color(*v);
            if (auto v = it->find("fill_color"); v != it->end()) n.fill_color = parse_color(*v);
            if (auto v = it->find("orientation"); v != it->end() && v->is_string())
                n.orientation = parse_orientation(v->get<std::string>());
            if (auto v = it->find("direction"); v != it->end() && v->is_string())
                n.direction = parse_direction(v->get<std::string>());
        }
        return &n;
    }
    if (type == "image") {
        auto& n = parent.add_child<Image>();
        n.rect   = rect;
        n.source = jn.value("source", "");
        if (auto it = jn.find("style"); it != jn.end() && it->is_object()) {
            if (auto v = it->find("tint"); v != it->end()) n.tint = parse_color(*v);
        }
        return &n;
    }
    if (type == "button") {
        auto& n = parent.add_child<Button>();
        n.rect = rect;
        if (auto v = jn.find("enabled"); v != jn.end() && v->is_boolean()) n.enabled = v->get<bool>();
        if (auto it = jn.find("style"); it != jn.end() && it->is_object()) {
            if (auto v = it->find("bg");       v != it->end()) n.bg       = parse_color(*v);
            if (auto v = it->find("hover_bg"); v != it->end()) n.bg_hover = parse_color(*v);
            if (auto v = it->find("press_bg"); v != it->end()) n.bg_press = parse_color(*v);
        }
        return &n;
    }
    log::warn(TAG, "unknown node type '{}'", type);
    return nullptr;
}

void parse_node(const nlohmann::json& jn, Node& parent, const Rect& parent_rect) {
    if (!jn.is_object()) { log::warn(TAG, "node entry is not an object"); return; }

    auto anchor = parse_anchor(jn.value("anchor", "tl"));
    f32 x = jn.value("x", 0.0f);
    f32 y = jn.value("y", 0.0f);
    f32 w = jn.value("w", 0.0f);
    f32 h = jn.value("h", 0.0f);
    Rect rect = resolve_rect(parent_rect, anchor, x, y, w, h);

    Node* node = build_atom(jn, parent, rect);
    if (!node) return;

    if (auto idv = jn.find("id"); idv != jn.end() && idv->is_string()) {
        node->id = idv->get<std::string>();
    }
    if (auto v = jn.find("visible"); v != jn.end() && v->is_boolean()) {
        node->visible = v->get<bool>();
    }
    if (auto ov = jn.find("owner_player"); ov != jn.end() && ov->is_number_unsigned()) {
        node->owner_player = ov->get<u32>();
    }

    if (auto it = jn.find("children"); it != jn.end() && it->is_array()) {
        for (const auto& child_jn : *it) {
            parse_node(child_jn, *node, node->rect);
        }
    }
}

} // namespace

bool load_from_json(Hud& hud, const nlohmann::json& doc,
                    u32 viewport_w, u32 viewport_h) {
    // Viewport feeds the composite block (the action-bar anchor resolves
    // against the viewport rect at load). Node templates don't need it
    // because CreateNode() supplies placement at instantiation time.
    if (!doc.is_object()) { log::warn(TAG, "hud.json root must be an object"); return false; }

    // Clear any previously-attached nodes so a fresh session re-loads cleanly.
    // (The root panel persists; its children are what we replace.)
    auto& root = hud.root();
    // There's no public clear method on Node; for now, re-assign its children
    // by moving them out. Since we only build up the tree here, clearing via
    // a fresh replacement is simplest — but Node children are std::unique_ptr
    // in a non-public vector, so we can't touch them directly. Workaround:
    // parse appends to whatever is there; callers are expected to either
    // (a) only call load_from_json once per session or (b) tear down the
    // entire Hud before re-loading. For v1 this is fine — the App calls
    // load at session start, and end_session resets the HUD on exit.
    (void)root;

    if (auto preset = doc.find("preset"); preset != doc.end() && preset->is_string()) {
        log::info(TAG, "hud preset: '{}'", preset->get<std::string>());
    }

    // composites block — engine-authored node groups: `action_bar`,
    // `command_bar`, `minimap`, `joystick`. Unknown keys are logged so
    // authoring typos don't fail silently.
    if (auto comps = doc.find("composites"); comps != doc.end() && comps->is_object()) {
        // Composite placements resolve against the HUD's logical (dp)
        // space — the same space Hud::begin_frame exposes each frame.
        // So a `br` anchor still lands at the physical corner; the
        // uniform px-per-dp scale is applied by the renderer at draw.
        f32 s = hud.ui_scale();
        if (s <= 0.0f) s = 1.0f;
        Rect viewport_rect{ 0.0f, 0.0f,
                            static_cast<f32>(viewport_w) / s,
                            static_cast<f32>(viewport_h) / s };

        if (auto ab = comps->find("action_bar"); ab != comps->end() && ab->is_object()) {
            ActionBarConfig cfg{};
            cfg.enabled = true;

            // Bar-level placement (anchor + offset + size against viewport).
            // Raw values are stashed so on_viewport_resized can re-resolve
            // the absolute rect without re-parsing the JSON.
            cfg.placement.anchor = parse_anchor(ab->value("anchor", "bc"));
            cfg.placement.x      = ab->value("x", 0.0f);
            cfg.placement.y      = ab->value("y", 0.0f);
            cfg.placement.w      = ab->value("w", 0.0f);
            cfg.placement.h      = ab->value("h", 0.0f);
            cfg.rect = resolve(viewport_rect, cfg.placement);

            // Style id selects the render prototype. `classic_rts` is the
            // only variant shipped today; unknown names fall through with
            // a warning so typos aren't silent. `style_params` overrides
            // the variant's default colors / sizes.
            if (auto sid = ab->find("style"); sid != ab->end() && sid->is_string()) {
                const std::string s = sid->get<std::string>();
                if (s == "classic_rts") {
                    cfg.style_id = ActionBarStyleId::ClassicRts;
                } else {
                    log::warn(TAG, "action_bar: unknown style '{}', using classic_rts", s);
                }
            }

            // Binding mode: who fills slots with abilities. Auto is the
            // RTS default (derived from selection); manual is Lua-driven
            // (action/MOBA-style maps that pick specific abilities).
            if (auto bm = ab->find("binding_mode"); bm != ab->end() && bm->is_string()) {
                const std::string s = bm->get<std::string>();
                if (s == "auto") {
                    cfg.binding_mode = ActionBarBindingMode::Auto;
                } else if (s == "manual") {
                    cfg.binding_mode = ActionBarBindingMode::Manual;
                } else {
                    log::warn(TAG, "action_bar: unknown binding_mode '{}', using auto", s);
                }
            }

            if (auto sp = ab->find("style_params"); sp != ab->end() && sp->is_object()) {
                if (auto v = sp->find("bg");                  v != sp->end()) cfg.style.bg                  = parse_color(*v);
                if (auto v = sp->find("cooldown_overlay");    v != sp->end()) cfg.style.cooldown_overlay    = parse_color(*v);
                if (auto v = sp->find("cooldown_text_color"); v != sp->end()) cfg.style.cooldown_text_color = parse_color(*v);
                if (auto v = sp->find("cooldown_text_size");  v != sp->end() && v->is_number())
                    cfg.style.cooldown_text_size = v->get<f32>();
                if (auto v = sp->find("hotkey_color");        v != sp->end()) cfg.style.hotkey_color        = parse_color(*v);
                if (auto v = sp->find("hotkey_badge_bg");     v != sp->end()) cfg.style.hotkey_badge_bg     = parse_color(*v);
                if (auto v = sp->find("disabled_tint");       v != sp->end()) cfg.style.disabled_tint       = parse_color(*v);
                if (auto v = sp->find("armed_border_color");  v != sp->end()) cfg.style.armed_border_color  = parse_color(*v);
                if (auto v = sp->find("armed_border_width");  v != sp->end() && v->is_number())
                    cfg.style.armed_border_width = v->get<f32>();
            }

            // Shared default style applied to every slot; per-slot
            // overrides layer on top.
            ActionBarSlotStyle default_slot_style{};
            if (auto ss = ab->find("slot_style"); ss != ab->end() && ss->is_object()) {
                if (auto v = ss->find("bg");           v != ss->end()) default_slot_style.bg           = parse_color(*v);
                if (auto v = ss->find("hover_bg");     v != ss->end()) default_slot_style.hover_bg     = parse_color(*v);
                if (auto v = ss->find("press_bg");     v != ss->end()) default_slot_style.press_bg     = parse_color(*v);
                if (auto v = ss->find("disabled_bg");  v != ss->end()) default_slot_style.disabled_bg  = parse_color(*v);
                if (auto v = ss->find("border_color"); v != ss->end()) default_slot_style.border_color = parse_color(*v);
                if (auto v = ss->find("border_width"); v != ss->end() && v->is_number())
                    default_slot_style.border_width = v->get<f32>();
            }

            if (auto slots = ab->find("slots"); slots != ab->end() && slots->is_array()) {
                for (const auto& js : *slots) {
                    if (!js.is_object()) continue;
                    ActionBarSlot slot{};
                    slot.style = default_slot_style;

                    slot.placement.anchor = parse_anchor(js.value("anchor", "tl"));
                    slot.placement.x      = js.value("x", 0.0f);
                    slot.placement.y      = js.value("y", 0.0f);
                    slot.placement.w      = js.value("w", 48.0f);
                    slot.placement.h      = js.value("h", 48.0f);
                    slot.rect = resolve(cfg.rect, slot.placement);

                    if (auto v = js.find("hotkey"); v != js.end() && v->is_string()) {
                        slot.hotkey = v->get<std::string>();
                    }

                    // Per-slot style override.
                    if (auto st = js.find("style"); st != js.end() && st->is_object()) {
                        if (auto v = st->find("bg");           v != st->end()) slot.style.bg           = parse_color(*v);
                        if (auto v = st->find("hover_bg");     v != st->end()) slot.style.hover_bg     = parse_color(*v);
                        if (auto v = st->find("press_bg");     v != st->end()) slot.style.press_bg     = parse_color(*v);
                        if (auto v = st->find("disabled_bg");  v != st->end()) slot.style.disabled_bg  = parse_color(*v);
                        if (auto v = st->find("border_color"); v != st->end()) slot.style.border_color = parse_color(*v);
                        if (auto v = st->find("border_width"); v != st->end() && v->is_number())
                            slot.style.border_width = v->get<f32>();
                    }

                    cfg.slots.push_back(std::move(slot));
                }
            }

            hud.set_action_bar_config(cfg);
            log::info(TAG, "action_bar: {} slots", cfg.slots.size());
        }

        if (auto mm = comps->find("minimap"); mm != comps->end() && mm->is_object()) {
            MinimapConfig cfg{};
            cfg.enabled = true;

            cfg.placement.anchor = parse_anchor(mm->value("anchor", "br"));
            cfg.placement.x      = mm->value("x", 0.0f);
            cfg.placement.y      = mm->value("y", 0.0f);
            cfg.placement.w      = mm->value("w", 0.0f);
            cfg.placement.h      = mm->value("h", 0.0f);
            cfg.rect = resolve(viewport_rect, cfg.placement);

            if (auto sid = mm->find("style"); sid != mm->end() && sid->is_string()) {
                const std::string s = sid->get<std::string>();
                if (s == "classic_rts") {
                    cfg.style_id = MinimapStyleId::ClassicRts;
                } else {
                    log::warn(TAG, "minimap: unknown style '{}', using classic_rts", s);
                }
            }

            if (auto sp = mm->find("style_params"); sp != mm->end() && sp->is_object()) {
                if (auto v = sp->find("bg");                v != sp->end()) cfg.style.bg                = parse_color(*v);
                if (auto v = sp->find("border_color");      v != sp->end()) cfg.style.border_color      = parse_color(*v);
                if (auto v = sp->find("border_width");      v != sp->end() && v->is_number())
                    cfg.style.border_width = v->get<f32>();
                if (auto v = sp->find("own_dot_color");     v != sp->end()) cfg.style.own_dot_color     = parse_color(*v);
                if (auto v = sp->find("ally_dot_color");    v != sp->end()) cfg.style.ally_dot_color    = parse_color(*v);
                if (auto v = sp->find("enemy_dot_color");   v != sp->end()) cfg.style.enemy_dot_color   = parse_color(*v);
                if (auto v = sp->find("neutral_dot_color"); v != sp->end()) cfg.style.neutral_dot_color = parse_color(*v);
                if (auto v = sp->find("dot_size");          v != sp->end() && v->is_number())
                    cfg.style.dot_size = v->get<f32>();
            }

            hud.set_minimap_config(cfg);
            log::info(TAG, "minimap: registered ({}x{})",
                      static_cast<i32>(cfg.rect.w), static_cast<i32>(cfg.rect.h));
        }

        if (auto cb = comps->find("command_bar"); cb != comps->end() && cb->is_object()) {
            CommandBarConfig cfg{};
            cfg.enabled = true;

            cfg.placement.anchor = parse_anchor(cb->value("anchor", "br"));
            cfg.placement.x      = cb->value("x", 0.0f);
            cfg.placement.y      = cb->value("y", 0.0f);
            cfg.placement.w      = cb->value("w", 0.0f);
            cfg.placement.h      = cb->value("h", 0.0f);
            cfg.rect = resolve(viewport_rect, cfg.placement);

            if (auto sp = cb->find("style_params"); sp != cb->end() && sp->is_object()) {
                if (auto v = sp->find("bg");               v != sp->end()) cfg.style.bg              = parse_color(*v);
                if (auto v = sp->find("hotkey_color");     v != sp->end()) cfg.style.hotkey_color    = parse_color(*v);
                if (auto v = sp->find("hotkey_badge_bg");  v != sp->end()) cfg.style.hotkey_badge_bg = parse_color(*v);
            }

            CommandBarSlotStyle default_slot_style{};
            if (auto ss = cb->find("slot_style"); ss != cb->end() && ss->is_object()) {
                if (auto v = ss->find("bg");           v != ss->end()) default_slot_style.bg           = parse_color(*v);
                if (auto v = ss->find("hover_bg");     v != ss->end()) default_slot_style.hover_bg     = parse_color(*v);
                if (auto v = ss->find("press_bg");     v != ss->end()) default_slot_style.press_bg     = parse_color(*v);
                if (auto v = ss->find("border_color"); v != ss->end()) default_slot_style.border_color = parse_color(*v);
                if (auto v = ss->find("border_width"); v != ss->end() && v->is_number())
                    default_slot_style.border_width = v->get<f32>();
            }

            if (auto slots = cb->find("slots"); slots != cb->end() && slots->is_array()) {
                for (const auto& js : *slots) {
                    if (!js.is_object()) continue;
                    CommandBarSlot slot{};
                    slot.style = default_slot_style;

                    slot.placement.anchor = parse_anchor(js.value("anchor", "tl"));
                    slot.placement.x      = js.value("x", 0.0f);
                    slot.placement.y      = js.value("y", 0.0f);
                    slot.placement.w      = js.value("w", 48.0f);
                    slot.placement.h      = js.value("h", 48.0f);
                    slot.rect = resolve(cfg.rect, slot.placement);

                    slot.command = js.value("command", "");
                    slot.icon    = js.value("icon",    "");
                    slot.hotkey  = js.value("hotkey",  "");

                    if (auto st = js.find("style"); st != js.end() && st->is_object()) {
                        if (auto v = st->find("bg");           v != st->end()) slot.style.bg           = parse_color(*v);
                        if (auto v = st->find("hover_bg");     v != st->end()) slot.style.hover_bg     = parse_color(*v);
                        if (auto v = st->find("press_bg");     v != st->end()) slot.style.press_bg     = parse_color(*v);
                        if (auto v = st->find("border_color"); v != st->end()) slot.style.border_color = parse_color(*v);
                        if (auto v = st->find("border_width"); v != st->end() && v->is_number())
                            slot.style.border_width = v->get<f32>();
                    }

                    cfg.slots.push_back(std::move(slot));
                }
            }

            hud.set_command_bar_config(cfg);
            log::info(TAG, "command_bar: {} slots", cfg.slots.size());
        }

        if (auto jy = comps->find("joystick"); jy != comps->end() && jy->is_object()) {
            // Desktop skips the joystick entirely — keyboard pan and
            // click-to-select cover the same ground and a virtual stick
            // on a mouse-driven build is just visual noise.
            if (!hud.is_mobile()) {
                log::info(TAG, "joystick: skipped (non-mobile platform)");
                // Still eaten from the "not yet implemented" skim at
                // the bottom so authors don't get a spurious warning.
            } else {
            JoystickConfig cfg{};
            cfg.enabled = true;

            cfg.placement.anchor = parse_anchor(jy->value("anchor", "bl"));
            cfg.placement.x      = jy->value("x", 0.0f);
            cfg.placement.y      = jy->value("y", 0.0f);
            cfg.placement.w      = jy->value("w", 0.0f);
            cfg.placement.h      = jy->value("h", 0.0f);
            cfg.rect = resolve(viewport_rect, cfg.placement);

            // Optional activation region. If omitted, the base rect
            // doubles as the activation region (v1 behavior).
            if (auto act = jy->find("activation"); act != jy->end() && act->is_object()) {
                cfg.has_activation = true;
                cfg.activation_placement.anchor = parse_anchor(act->value("anchor", "bl"));
                cfg.activation_placement.x      = act->value("x", 0.0f);
                cfg.activation_placement.y      = act->value("y", 0.0f);
                cfg.activation_placement.w      = act->value("w", 0.0f);
                cfg.activation_placement.h      = act->value("h", 0.0f);
                cfg.activation_rect = resolve(viewport_rect, cfg.activation_placement);
            } else {
                cfg.activation_rect = cfg.rect;
            }

            if (auto sp = jy->find("style_params"); sp != jy->end() && sp->is_object()) {
                if (auto v = sp->find("base_color");        v != sp->end()) cfg.style.base_color        = parse_color(*v);
                if (auto v = sp->find("base_border");       v != sp->end()) cfg.style.base_border       = parse_color(*v);
                if (auto v = sp->find("base_border_width"); v != sp->end() && v->is_number())
                    cfg.style.base_border_width = v->get<f32>();
                if (auto v = sp->find("knob_color");        v != sp->end()) cfg.style.knob_color        = parse_color(*v);
                if (auto v = sp->find("knob_border");       v != sp->end()) cfg.style.knob_border       = parse_color(*v);
                if (auto v = sp->find("knob_border_width"); v != sp->end() && v->is_number())
                    cfg.style.knob_border_width = v->get<f32>();
                if (auto v = sp->find("knob_size_frac");    v != sp->end() && v->is_number())
                    cfg.style.knob_size_frac = v->get<f32>();
                if (auto v = sp->find("deadzone_frac");     v != sp->end() && v->is_number())
                    cfg.style.deadzone_frac = v->get<f32>();
                if (auto v = sp->find("idle_alpha_frac");   v != sp->end() && v->is_number())
                    cfg.style.idle_alpha_frac = v->get<f32>();
            }

            hud.set_joystick_config(cfg);
            log::info(TAG, "joystick: registered ({}x{}, activation {}x{})",
                      static_cast<i32>(cfg.rect.w), static_cast<i32>(cfg.rect.h),
                      static_cast<i32>(cfg.activation_rect.w),
                      static_cast<i32>(cfg.activation_rect.h));
            } // end mobile-only branch
        }

        // Skim remaining composite keys so authoring feedback is consistent.
        for (auto it = comps->begin(); it != comps->end(); ++it) {
            const std::string& k = it.key();
            if (k == "action_bar" || k == "minimap" || k == "command_bar" || k == "joystick") continue;
            log::info(TAG, "composite '{}' declared (not yet implemented)", k);
        }
    }

    // world_overlays block — entity-anchored screen-pixel-sized overlays
    // (bars, name labels). Entity bars are parsed here; other sub-blocks
    // (name label) land in follow-on sub-phases.
    if (auto wo = doc.find("world_overlays"); wo != doc.end() && wo->is_object()) {
        WorldOverlayConfig cfg{};
        if (auto eb = wo->find("entity_bars"); eb != wo->end() && eb->is_object()) {
            cfg.entity_bars_enabled = true;
            cfg.entity_bars.z_offset = eb->value("z_offset", 2.0f);
            cfg.entity_bars.spacing  = eb->value("spacing", 1u);
            if (auto sz = eb->find("size"); sz != eb->end() && sz->is_object()) {
                cfg.entity_bars.width  = sz->value("w", 30u);
                cfg.entity_bars.height = sz->value("h", 3u);
            }
            if (auto bars = eb->find("bars"); bars != eb->end() && bars->is_array()) {
                for (const auto& jb : *bars) {
                    if (!jb.is_object()) continue;
                    WorldBarTemplate tpl{};
                    tpl.state_id = jb.value("state", "");
                    if (tpl.state_id.empty()) { log::warn(TAG, "entity bar missing 'state'"); continue; }
                    // visibility: string or array of strings
                    auto parse_vp = [](std::string_view s) -> VisibilityPolicy {
                        if (s == "not_full") return VisibilityPolicy::NotFull;
                        if (s == "hovered")  return VisibilityPolicy::Hovered;
                        if (s == "selected") return VisibilityPolicy::Selected;
                        return VisibilityPolicy::Always;
                    };
                    if (auto v = jb.find("visibility"); v != jb.end()) {
                        if (v->is_string()) {
                            tpl.visibility.push_back(parse_vp(v->get<std::string>()));
                        } else if (v->is_array()) {
                            for (const auto& e : *v) {
                                if (e.is_string()) tpl.visibility.push_back(parse_vp(e.get<std::string>()));
                            }
                        }
                    }
                    if (auto st = jb.find("style"); st != jb.end() && st->is_object()) {
                        if (auto c = st->find("bg_color");   c != st->end()) tpl.style.bg_color   = parse_color(*c);
                        if (auto c = st->find("fill_color"); c != st->end()) tpl.style.fill_color = parse_color(*c);
                    }
                    cfg.entity_bars.bars.push_back(std::move(tpl));
                }
            }
        }
        if (auto nl = wo->find("unit_name_label"); nl != wo->end() && nl->is_object()) {
            cfg.name_label_enabled = true;
            cfg.name_label.z_offset = nl->value("z_offset", 170.0f);
            cfg.name_label.px_size  = nl->value("size",      14.0f);
            if (auto st = nl->find("style"); st != nl->end() && st->is_object()) {
                if (auto c = st->find("color");    c != st->end()) cfg.name_label.style.color    = parse_color(*c);
                if (auto c = st->find("bg_color"); c != st->end()) {
                    cfg.name_label.style.bg_color = parse_color(*c);
                    cfg.name_label.style.has_bg   = true;
                }
                if (auto b = st->find("has_bg"); b != st->end() && b->is_boolean()) {
                    cfg.name_label.style.has_bg = b->get<bool>();
                }
                if (auto p = st->find("bg_pad_x"); p != st->end() && p->is_number()) cfg.name_label.style.bg_pad_x = p->get<f32>();
                if (auto p = st->find("bg_pad_y"); p != st->end() && p->is_number()) cfg.name_label.style.bg_pad_y = p->get<f32>();
            }
        }
        hud.set_world_overlay_config(cfg);
    }

    // selection_marquee block — per-map colors for the box-drag
    // rectangle the RTS preset draws when frame-selecting units.
    // Default (no block): transparent fill + green border, outline-only.
    if (auto sm = doc.find("selection_marquee"); sm != doc.end() && sm->is_object()) {
        Hud::MarqueeStyle ms{};
        if (auto v = sm->find("fill");   v != sm->end()) ms.fill   = parse_color(*v);
        if (auto v = sm->find("border"); v != sm->end()) ms.border = parse_color(*v);
        hud.set_marquee_style(ms);
    }

    // nodes block — each entry is registered as a TEMPLATE keyed by its
    // id. Templates are definitions; they don't render until a Lua script
    // calls CreateNode(template_id) to instantiate one. A hud.json with
    // declared nodes but no Lua calling CreateNode renders no custom UI.
    hud.clear_node_templates();
    if (auto nodes = doc.find("nodes"); nodes != doc.end() && nodes->is_array()) {
        for (const auto& entry : *nodes) {
            if (!entry.is_object()) continue;
            auto idv = entry.find("id");
            if (idv == entry.end() || !idv->is_string()) {
                log::warn(TAG, "top-level node entry missing 'id'; skipped");
                continue;
            }
            hud.add_node_template(idv->get<std::string>(), entry);
        }
    }
    return true;
}

// ── Template instantiation (called from Lua via CreateNode) ──────────────
// Looks up a registered template by id and builds its node subtree under
// the HUD's root. Fails (returns false) if the template is unknown or a
// node with the same id is already alive. Placement is always Lua-supplied
// at the root; the template JSON's root may omit anchor/x/y/w/h.
bool instantiate_template(Hud& hud, std::string_view template_id,
                          u32 viewport_w, u32 viewport_h,
                          const TemplatePlacement& placement) {
    if (template_id.empty()) return false;
    if (hud.find_node_by_id(template_id)) {
        log::warn(TAG, "CreateNode: id '{}' already exists; call destroy first", template_id);
        return false;
    }
    const nlohmann::json* spec = hud.get_node_template(template_id);
    if (!spec) {
        log::warn(TAG, "CreateNode: template '{}' not found", template_id);
        return false;
    }
    // Inject placement at the root. Clone the template so the registry
    // stays untouched — future CreateNode calls may use different placement.
    nlohmann::json patched = *spec;
    patched["anchor"]       = std::string(placement.anchor);
    patched["x"]            = placement.x;
    patched["y"]            = placement.y;
    patched["w"]            = placement.w;
    patched["h"]            = placement.h;
    patched["owner_player"] = placement.owner_player;

    // Same physical→dp conversion as the composite parser above so
    // Lua-supplied placement coords are interpreted in dp, not raw
    // framebuffer pixels.
    f32 s = hud.ui_scale();
    if (s <= 0.0f) s = 1.0f;
    Rect viewport{ 0.0f, 0.0f,
                   static_cast<f32>(viewport_w) / s,
                   static_cast<f32>(viewport_h) / s };
    parse_node(patched, hud.root(), viewport);
    return true;
}

bool load_from_asset(Hud& hud, std::string_view asset_path, u32 viewport_w, u32 viewport_h) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) { log::warn(TAG, "AssetManager not initialized"); return false; }
    auto bytes = mgr->read_file_bytes(asset_path);
    if (bytes.empty()) {
        // Not an error — maps without HUD customization are legal.
        return false;
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception& e) {
        log::error(TAG, "hud.json parse failed ({}): {}", asset_path, e.what());
        return false;
    }
    return load_from_json(hud, doc, viewport_w, viewport_h);
}

} // namespace uldum::hud
