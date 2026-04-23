#pragma once

#include "core/types.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// Forward-decls: keep the header free of vulkan.h / VMA.
typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::hud {

class Node;                    // node.h
class Panel;                   // node.h
struct WorldOverlayConfig;     // world.h
struct WorldContext;           // world.h
struct TextTagId;              // text_tag.h
struct TextTagCreateInfo;      // text_tag.h

// Packed RGBA color, 0xAABBGGRR on little-endian hosts. Use rgba() helper
// to build one; the HUD pipeline expects u8×4 → normalized vec4 with the
// R component first.
struct Color {
    u32 rgba = 0xFFFFFFFF;
};
inline constexpr Color rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return { static_cast<u32>(r)
           | (static_cast<u32>(g) <<  8)
           | (static_cast<u32>(b) << 16)
           | (static_cast<u32>(a) << 24) };
}

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
};

// The HUD subsystem. Owns the Vulkan pipeline, pooled ring buffers for
// vertex / index data, and a 1×1 white texture used for non-textured
// quads. Rendered as an overlay inside the main render pass after the
// 3D scene.
//
// Lifecycle per frame:
//   hud.begin_frame(w, h)        — reset CPU-side draw lists, stash viewport
//   hud.draw_rect(...) etc.       — record quads
//   hud.render(cmd)              — upload, bind pipeline, draw (inside render pass)
//
// Stage A scope: solid-color rectangles only. Widget tree, text, input,
// and platform-specific affordances land in subsequent stages.
class Hud {
public:
    Hud();
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Local player for this process (engine-internal — NOT Lua-visible).
    // Used as a render / sync filter so owned-by-other-player nodes don't
    // appear on this user's screen. Set from App::start_session. Dedicated
    // servers never render anyway, so the value there is moot.
    void set_local_player(u32 player_id);
    u32  local_player() const;

    // ── Per-frame ────────────────────────────────────────────────────────
    // begin_frame() resets the CPU-side draw list and stashes the viewport.
    // render() uploads to the GPU ring buffer and issues draw calls inside
    // the caller's render pass. Tree widgets are drawn via hud.root().draw()
    // — callers don't iterate explicitly.
    void begin_frame(u32 screen_w, u32 screen_h);
    void render(VkCommandBuffer cmd);

    // Low-level primitive — draw a solid-color rectangle. Used by widgets
    // internally; callers usually add Panel / Button nodes via the tree
    // instead.
    void draw_rect(const Rect& r, Color color);

    // Low-level primitive — draw a texture, stretched to fill the rect.
    // `asset_path` is resolved by the asset manager (KTX2 / PNG); the
    // first call for a path uploads the image and caches its GPU
    // resources for the Hud's lifetime, subsequent calls just bind the
    // cached descriptor. `tint` is a multiplier (default opaque white).
    // No-ops if the asset is missing or fails to decode.
    void draw_image(const Rect& r, std::string_view asset_path,
                    Color tint = rgba(255, 255, 255, 255));

    // Low-level primitive — draw a single line of UTF-8 text using the
    // default HUD font. Position is the left end of the text baseline (in
    // screen pixels). px_size selects on-screen glyph size; the MSDF
    // atlas was authored at one size but scales cleanly. No-ops silently
    // if the font failed to load or the atlas is full.
    void draw_text(f32 x_left, f32 y_baseline, std::string_view utf8,
                   Color color, f32 px_size);

    // Line metrics of the default UI font, in on-screen pixels at the
    // given target size. Used by Label and other text-bearing nodes to
    // position the baseline and align vertically.
    f32 text_ascent_px(f32 px_size)      const;
    f32 text_line_height_px(f32 px_size) const;

    // Measure the pixel width a UTF-8 string would occupy at the given
    // target size. Used by Label for center / right alignment. Returns 0
    // if the font failed to load.
    f32 text_width_px(std::string_view utf8, f32 px_size) const;

    // ── Widget tree ──────────────────────────────────────────────────────
    // One persistent root Panel is created at init; all widgets attach as
    // descendants of it. Cleared of children on shutdown.
    Panel& root();

    // Remove all nodes under the root. Called between sessions so a freshly
    // loaded hud.json starts from a clean slate. Also clears transient
    // input state (hover / pressed pointers) because those might reference
    // nodes we're about to free.
    void clear_nodes();

    // Walk the tree and return the first node with the matching JSON id.
    // O(N) in the number of nodes — N is small in practice (tens to a few
    // hundred); revisit with a hash map if profiling shows hot lookups.
    // Returns nullptr if not found.
    Node* find_node_by_id(std::string_view id);

    // Walk the tree and destroy the first node with a matching id (and
    // its entire subtree). Returns true if a node was removed. Used by
    // Lua's DestroyNode(id); safe to call on unknown ids (returns false).
    bool remove_node_by_id(std::string_view id);

    // ── By-id state setters (Lua-facing; also route to MP sync) ─────────
    // Each resolves the node by id and applies the change locally; if a
    // sync_fn is installed (host mode), it also emits a protocol message
    // tagged with the node's owner so the network layer can route it.
    // Silent no-op on unknown id or wrong node type.
    void set_label_text(std::string_view id, std::string_view text);
    void set_bar_fill(std::string_view id, f32 fill);
    void set_node_visible(std::string_view id, bool visible);
    void set_image_source(std::string_view id, std::string_view source);
    void set_button_enabled(std::string_view id, bool enabled);

    // ── Templates (hud.json `nodes` block) ───────────────────────────────
    // Templates are definitions — they describe what a node subtree looks
    // like but don't render until Lua instantiates them. At map load the
    // hud.json loader registers each top-level entry in the `nodes` array
    // as a template keyed by its id. Lua calls `CreateNode(id)` which
    // looks up the template and builds a fresh instance under the root.
    void add_node_template(std::string id, const nlohmann::json& spec);
    const nlohmann::json* get_node_template(std::string_view id) const;
    void clear_node_templates();

    // Instantiate a registered template by id into the HUD root at the
    // given placement. Returns true on success. Fails if the template
    // doesn't exist or a node with the same id is already present.
    // Called from Lua's CreateNode(id, { anchor, x, y, w, h }).
    struct Placement {
        std::string_view anchor = "tl";
        f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        u32 owner_player = UINT32_MAX;   // broadcast by default
    };
    bool instantiate_template(std::string_view id, const Placement& placement);

    // Walk the tree, hit-test + draw the visible subtree, and record quads
    // for this frame. Called between begin_frame() and render(). No-op if
    // the root is empty.
    void draw_tree();

    // ── World UI ─────────────────────────────────────────────────────────
    // Configuration is set once at map load (from hud.json's
    // `world_overlays` block). Context is set at session start / cleared at
    // session end; it carries refs into the active sim, camera, picker,
    // selection, terrain. draw_world_overlays() walks all entities each
    // frame and emits quads into the same batcher as screen UI.
    void set_world_overlay_config(const WorldOverlayConfig& cfg);
    void set_world_context(const WorldContext* ctx);
    // `alpha` is the sub-tick interpolation factor (0..1) — passed through
    // to the projection so world-anchored overlays follow smoothly-moving
    // units, matching how `Renderer::draw` interpolates the units themselves.
    void draw_world_overlays(f32 alpha);

    // Advance text-tag animation state (velocity × dt, age, fade).
    // Called once per frame before draw_world_overlays; expired tags
    // (`age >= lifespan` with `lifespan > 0`) are released.
    void update_text_tags(f32 dt);

    // ── Text tags ────────────────────────────────────────────────────────
    // WC3-style floating / persistent text. Engine-side API — Lua
    // bindings in 16c-v forward here. Returns an invalid id if the pool
    // is exhausted or the info is degenerate.
    TextTagId create_text_tag(const TextTagCreateInfo& info);
    void      destroy_text_tag(TextTagId id);
    bool      text_tag_alive(TextTagId id) const;

    void set_text_tag_text    (TextTagId id, std::string_view text);
    void set_text_tag_pos     (TextTagId id, f32 x, f32 y, f32 z);
    void set_text_tag_pos_unit(TextTagId id, u32 unit_id, f32 z_offset);
    void set_text_tag_color   (TextTagId id, Color color);
    void set_text_tag_velocity(TextTagId id, f32 vx, f32 vy);
    void set_text_tag_visible (TextTagId id, bool visible);

    // ── Network sync (host-side) ─────────────────────────────────────────
    // Host installs a callback that turns local HUD mutations into
    // protocol messages and sends them to the appropriate client peer(s).
    // The callback receives:
    //   - an opaque byte packet (ready-to-send)
    //   - the owning player (UINT32_MAX = broadcast, else target slot)
    // Offline sessions and clients leave this unset; mutations stay local.
    using SyncFn = std::function<void(const std::vector<u8>& packet, u32 owner_player)>;
    void set_sync_fn(SyncFn fn);

    // ── Button event callback (for input → event routing) ─────────────────
    // Called synchronously from Button::on_release when a click completes
    // over the button. App wires this to the server's script engine
    // (host) or to the network send path (client).
    using ButtonEventFn = std::function<void(const std::string& node_id)>;
    void set_button_event_fn(ButtonEventFn fn);
    // Internal: invoked by Button::on_release; exposed so node.cpp can call
    // without needing to know about Impl or the callback storage.
    void fire_button_event(const std::string& node_id);

    // ── Input ────────────────────────────────────────────────────────────
    // The platform layer polls mouse / touch each frame and forwards the
    // resulting (x, y, button_down) here. The HUD tracks hover and press
    // edges internally, fires widget callbacks, and exposes input_captured()
    // so gameplay input can skip clicks that a widget handled.
    void handle_pointer(f32 x, f32 y, bool button_down);
    bool input_captured() const;

    // Most recent pointer coords — set by handle_pointer each frame.
    // Exposed so world-UI code can run hit tests (Picker::pick_target etc.)
    // against the same position the HUD is using for hover / press state.
    f32 pointer_x() const;
    f32 pointer_y() const;

    // Opaque state — declared in hud.cpp to keep Vulkan / VMA out of the header.
    struct Impl;
private:
    Impl* m_impl = nullptr;
};

} // namespace uldum::hud
