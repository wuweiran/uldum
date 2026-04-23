#include "hud/hud_loader.h"

#include "hud/hud.h"
#include "hud/node.h"
#include "hud/world.h"
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

// 9-point anchor → (horizontal_fraction, vertical_fraction) against parent.
// l/c/r map to 0/0.5/1.0; t/m/b map to 0/0.5/1.0. Values outside the 9-
// point vocabulary default to top-left.
struct AnchorFrac { f32 h; f32 v; };
AnchorFrac parse_anchor(std::string_view s) {
    // Fixed 2-character codes: first char is vertical, second is horizontal.
    // Match the ui.md convention: tl, tc, tr, ml, mc, mr, bl, bc, br.
    if (s.size() != 2) return { 0.0f, 0.0f };
    f32 v = (s[0] == 'm') ? 0.5f : (s[0] == 'b') ? 1.0f : 0.0f;  // t default
    f32 h = (s[1] == 'c') ? 0.5f : (s[1] == 'r') ? 1.0f : 0.0f;  // l default
    return { h, v };
}

// Resolve a (parent, anchor, offset, size) into an absolute rect. The
// anchor picks a point on the parent rect AND a matching corner on the
// child; the child's anchor corner is placed at the parent's anchor
// point plus the (x, y) offset.
Rect resolve_rect(const Rect& parent, AnchorFrac a, f32 x, f32 y, f32 w, f32 h) {
    Rect r{};
    r.x = parent.x + parent.w * a.h - w * a.h + x;
    r.y = parent.y + parent.h * a.v - h * a.v + y;
    r.w = w;
    r.h = h;
    return r;
}

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
                    u32 /*viewport_w*/, u32 /*viewport_h*/) {
    // Viewport is unused here now that `nodes` entries are stored as
    // templates rather than parsed into live nodes at load. It's still
    // taken as a parameter for API compatibility and in case later
    // sections add load-time layout work.
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

    // composites block — recognized but not applied yet (composites land
    // in a later sub-phase). Log whatever keys are present so authors get
    // feedback while authoring.
    if (auto comps = doc.find("composites"); comps != doc.end() && comps->is_object()) {
        for (auto it = comps->begin(); it != comps->end(); ++it) {
            log::info(TAG, "composite '{}' declared (not yet implemented)", it.key());
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

    Rect viewport{ 0.0f, 0.0f, static_cast<f32>(viewport_w), static_cast<f32>(viewport_h) };
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
