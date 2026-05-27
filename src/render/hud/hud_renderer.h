#pragma once

// HudRenderer — Vulkan-bound visual layer that consumes the platform-
// agnostic Hud data model (in src/hud/). After Phase 23, the renderer
// owns every Vulkan-touching field and method that used to live on Hud;
// state-mutating callers (App, script bindings, hud_network) keep
// talking to Hud unchanged.
//
// Lifecycle:
//   renderer.init(hud, rhi)          once at engine bring-up
//   renderer.shutdown()              once at engine teardown
//   renderer.begin_frame(w, h)       per frame, before any draw_* call
//   renderer.draw_world_overlays(a)  per frame, world-anchored elements
//   renderer.draw_tree()             per frame, walks Hud's nodes
//   renderer.render(cmd)             per frame, inside the render pass
//
// HudRenderer is declared `friend class HudRenderer` of Hud so it can
// reach into Hud::Impl for the per-frame tree walk + composite state
// reads (action bar slot configs etc.). State mutators stay on Hud.

#include "core/types.h"
#include "hud/hud.h"  // Rect, Color, rgba — public POD types
#include "rhi/command_list.h"

#include <string_view>

namespace uldum::rhi { class Rhi; }

namespace uldum::hud {

struct WorldOverlayConfig;

class HudRenderer : public HudRenderInterface {
public:
    HudRenderer();
    ~HudRenderer() override;
    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    // Bind to a Hud instance + Vulkan device. Hud must outlive
    // HudRenderer. Creates pipelines, descriptor pool, ring buffers,
    // 1×1 white texture, and loads the system font.
    bool init(Hud& hud, rhi::Rhi& rhi);
    void shutdown();

    // Drop the per-session image cache. HUD icons are looked up by
    // map-relative paths (e.g. "textures/icons/attack.ktx2"); the same
    // key resolves to different bytes across maps, so a stale cache
    // entry from the previous map shows the wrong icon. Called by
    // App::end_session between maps.
    void reset_session_images();

    // begin_frame() resets CPU-side draw lists and stashes the viewport
    // (physical framebuffer pixels). It also updates the bound Hud's
    // logical screen_w/h and root rect so node layout / hit tests see
    // the same coordinate space for the frame.
    void begin_frame(u32 screen_w, u32 screen_h);

    // Walk Hud's node tree + composites, emitting draw commands into the
    // batcher. Composites (action_bar, command_bar, minimap, joystick,
    // inventory, display_message, tooltip) draw inside this call after
    // the node tree, matching the old Hud::draw_tree order.
    void draw_tree();

    // Walk world-anchored overlays (entity bars, name labels, text tags)
    // for the current frame. `alpha` is the sub-tick interpolation factor.
    void draw_world_overlays(f32 alpha);

    // Low-level primitives (overrides of HudRenderInterface).
    void draw_rect(const Rect& r, Color color) override;
    void draw_marquee(f32 x0, f32 y0, f32 x1, f32 y1);
    void draw_image(const Rect& r, std::string_view asset_path,
                    Color tint) override;
    void draw_image_disc(f32 cx, f32 cy, f32 radius,
                         std::string_view asset_path,
                         Color tint = rgba(255, 255, 255, 255));
    void draw_text(f32 x_left, f32 y_baseline, std::string_view utf8,
                   Color color, f32 px_size) override;

    // Font metric queries — used by labels, badges, tooltips for layout.
    f32 text_ascent_px(f32 px_size)      const override;
    f32 text_line_height_px(f32 px_size) const override;
    f32 text_width_px(std::string_view utf8, f32 px_size) const override;

    // Upload batches, bind pipelines, issue draws. Call inside the
    // render pass; invalidates `frame_open` so subsequent draw_* calls
    // are silent no-ops until the next begin_frame.
    void render(rhi::CommandList& cmd);

    // Bound Hud — read-only accessor. Tree-walking node draws need it
    // to query local_player / locale_manager. Overrides HudRenderInterface.
    Hud& hud() const override { return *m_hud; }

    // Opaque state.
    struct Impl;

private:
    Impl* m_impl = nullptr;
    Hud*  m_hud  = nullptr;
};

} // namespace uldum::hud
