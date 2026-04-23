#pragma once

#include "core/types.h"
#include "hud/hud.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uldum::hud {

// Base node. Owns its children; laid out in absolute screen-pixel coords
// for v1 (relative layout and auto-sizing land later). Input routing
// walks the tree back-to-front — children drawn later sit on top and are
// hit-tested first.
class Node {
public:
    // Stable identifier for Lua addressing. Authored in hud.json for
    // declarative nodes, assigned at create time for runtime-created
    // nodes. Empty for anonymous nodes (they still render but can't be
    // addressed by Lua).
    std::string id;
    Rect rect{};           // absolute screen-pixel rect
    bool visible      = true;
    // When false, the node + its rect don't participate in hit testing —
    // clicks fall through to whatever lies beneath. Set false on pure-
    // visual atoms (labels, images, bars) so they don't steal input from
    // their interactive parent (a button with a centered label child).
    bool hit_testable = true;

    // MP ownership: which player's HUD this node lives in. UINT32_MAX =
    // broadcast (every connected client sees it). Assigned at creation
    // from the Lua `owner` field; the host's sync callback filters
    // network messages by this.
    u32 owner_player = UINT32_MAX;

    virtual ~Node() = default;

    // Factory: construct a child in place, return a typed reference so
    // callers can keep configuring (rect, style, etc.).
    template <typename T, typename... Args>
    T& add_child(Args&&... args) {
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *node;
        m_children.push_back(std::move(node));
        return ref;
    }

    const std::vector<std::unique_ptr<Node>>& children() const { return m_children; }

    // Drop all direct children (and, transitively, their subtrees). Used
    // by Hud to reset the tree between sessions so a freshly-loaded
    // hud.json starts from an empty root.
    void clear_children() { m_children.clear(); }

    // Recursively find + remove a descendant by id. Returns true if a
    // node was erased. Used by Hud::remove_node_by_id (Lua DestroyNode).
    bool erase_child_by_id(std::string_view target_id) {
        for (auto it = m_children.begin(); it != m_children.end(); ++it) {
            if ((*it)->id == target_id) {
                m_children.erase(it);
                return true;
            }
            if ((*it)->erase_child_by_id(target_id)) return true;
        }
        return false;
    }

    // Default: rectangular hit test. Override for non-rect shapes (circles,
    // mask-based icons). Respects `hit_testable`. The owner filter is
    // applied externally during tree walk (hit_test_tree in hud.cpp) so
    // the hit-test implementation itself stays shape-focused.
    virtual bool hit_test(f32 x, f32 y) const {
        return visible && hit_testable
            && x >= rect.x && x < rect.x + rect.w
            && y >= rect.y && y < rect.y + rect.h;
    }

    // True iff this node is visible to the given local player. Broadcast
    // (owner == UINT32_MAX) means visible to everyone.
    bool is_owned_by(u32 player_id) const {
        return owner_player == UINT32_MAX || owner_player == player_id;
    }

    // Draw self then children (children on top). Called inside Hud::render().
    virtual void draw(Hud& hud) const;

    // Input callbacks — default no-op. Interactive nodes (Button and any
    // future composite) override to track state and fire user callbacks.
    // on_release returns true if this release completed a click (pressed
    // + released over the same node + the node is interactive). The Hud
    // uses that to dispatch a node event (`fire_button_event`).
    virtual void on_hover_change(bool /*hovered*/) {}
    virtual void on_press()                        {}
    virtual bool on_release(bool /*over_node*/)    { return false; }  // over_node = pointer still over this node at release

protected:
    std::vector<std::unique_ptr<Node>> m_children;
};

// ── Atom nodes ────────────────────────────────────────────────────────────
// Five primitives from which all map-authored HUD content is composed:
// panel, label, image, bar, button. Each has a style block (frozen at
// creation) and a content block (Lua-mutable at runtime).

// Plain rectangle with a background color. No input behavior. Used as a
// container or a decorative surface.
class Panel : public Node {
public:
    Color bg = rgba(30, 30, 38, 220);
    void draw(Hud& hud) const override;
};

// Non-interactive text. Single line (multi-line wraps later when the line
// breaker lands). `baseline_y_offset` places the baseline relative to
// `rect.y` — defaults to near the top of the rect.
class Label : public Node {
public:
    enum class Align : u8 { Left, Center, Right };

    std::string text;
    Color       color     = rgba(255, 255, 255);
    f32         px_size   = 16.0f;
    Align       align     = Align::Left;

    Label() { hit_testable = false; }
    void draw(Hud& hud) const override;
};

// Fill-fraction rectangle (health / mana / cast progress / cooldown).
// `fill` in [0, 1]; `orientation` and `direction` control which edge the
// bar grows from. For v1, horizontal LTR and vertical BTT are the common
// cases; RTL / TTB are supported but less tested.
class Bar : public Node {
public:
    enum class Orientation : u8 { Horizontal, Vertical };
    enum class Direction   : u8 { Ltr, Rtl, Btt, Ttb };

    Color       bg_color   = rgba(20, 20, 24, 220);
    Color       fill_color = rgba(60, 200, 80, 240);
    Orientation orientation = Orientation::Horizontal;
    Direction   direction   = Direction::Ltr;
    f32         fill        = 1.0f;   // [0, 1]

    Bar() { hit_testable = false; }
    void draw(Hud& hud) const override;
};

// Textured quad. `source` is an asset path (KTX2 / PNG) resolved at draw
// time via Hud's cached image loader. `tint` is a color multiplier
// applied to each sampled texel (opaque white leaves the image unchanged).
// Fit modes (stretch / contain / cover) are deferred — v1 always stretches
// the image to fill the rect.
class Image : public Node {
public:
    std::string source;
    Color       tint = rgba(255, 255, 255, 255);

    Image() { hit_testable = false; }
    void draw(Hud& hud) const override;
};

// Clickable rectangle with hover / press visual feedback. on_release()
// with over==true fires the click callback; handlers defined in hud.cpp.
class Button : public Node {
public:
    Color bg       = rgba(60,  64,  80,  240);
    Color bg_hover = rgba(80,  84, 104,  250);
    Color bg_press = rgba(110, 114, 140, 255);
    bool  enabled  = true;

    // Direct callback — used for engine-internal smoke tests. The
    // canonical Lua path routes through `Hud::fire_button_event(id)`
    // → script engine trigger (EVENT_BUTTON_PRESSED).
    std::function<void()> on_click_cb;

    bool hovered() const { return m_hovered; }
    bool pressed() const { return m_pressed; }

    void draw(Hud& hud) const override;
    void on_hover_change(bool h) override { m_hovered = h; }
    void on_press()              override { if (enabled) m_pressed = true; }
    bool on_release(bool over)   override;

private:
    bool m_hovered = false;
    bool m_pressed = false;
};

} // namespace uldum::hud
