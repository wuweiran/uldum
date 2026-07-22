#pragma once

#include "core/types.h"
#include "render/effect.h"   // GlowParams
#include "rhi/handles.h"
#include "rhi/command_list.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>
#include <vector>

namespace uldum::rhi { class Rhi; }

namespace uldum::render {

// Life envelope for a glow: 0→1→0 over `age`. Unset fade_in/fade_out default
// to a quick rise (10% of life) + long tail (20%) with a full-brightness hold;
// smoothstep-eased. Shared by the shaft (erosion factor) and the point light
// (brightness) so both ride the same curve.
f32 glow_envelope(f32 age, const GlowParams& params);

// GPU vertex for a glow shaft quad. No texture/shape id — the glow shader is
// single-purpose (procedural Tyndall), driven by UV + color + per-shaft
// tyndall strength + the fade envelope (drives volumetric erosion in the
// shader, not a flat alpha fade).
struct GlowVertex {
    glm::vec3 position;
    glm::vec4 color;      // rgb = tint; a = static brightness cap (intensity·color.a)
    glm::vec2 texcoord;   // U across width (0..1), V up the shaft (0 base, 1 top)
    f32       tyndall;    // striation strength for this shaft
    f32       fade;       // 0→1→0 envelope (raw), drives volumetric erosion
};

// A live glow effect. The engine's general light renderer — today it draws a
// single STATIC vertical volumetric light shaft that fades in then out (a door
// opening/closing in the dark); future use (persistent "hero glow" on a unit,
// etc.) extends this system rather than the particles. One "glow"-type effect
// spawns one GlowFx, which renders one shaft at the anchor, fades over
// `params.life` (no motion), then dies. Emits one point light.
struct GlowFx {
    u32 id = 0;              // stable handle — unit-attached glows move the anchor
    glm::vec3 base{0};       // ground anchor (world)
    GlowParams params{};
    glm::vec4 color{1};      // single tint (no start→end lerp)
    f32 age = 0;
};

// Engine-owned glow/light backend. Sibling of ParticleSystem: own dynamic
// VBO, additive pipeline (created by Renderer), same init/update/upload/draw/
// clear lifecycle. NOT a particle system — no gravity/spread; a glow is a
// single static shaft (and, later, other engine-authored light visuals).
class GlowSystem {
public:
    static constexpr u32 MAX_GLOWS = 256;   // one shaft each

    bool init(rhi::Rhi& rhi);
    void shutdown();

    // Spawn a glow effect anchored at `base`, tinted `color`. Returns a stable
    // handle the caller keeps to move the anchor (set_base) while it lives — so
    // a unit-attached glow follows the unit. 0 if the pool is full.
    u32 spawn(glm::vec3 base, const GlowParams& params, glm::vec4 color);

    // Move a live glow's anchor (unit-attached glows call this each frame).
    // Returns false if the glow is gone (faded out) — the caller uses this to
    // retire the owning effect instance.
    bool set_base(u32 id, glm::vec3 base);

    // Advance ages, drop dead glows. Call once per frame.
    void update(f32 dt);

    // Build shaft quads (camera-yaw-aligned) and upload. Call after update.
    void upload(glm::vec3 camera_pos);

    // Draw all shafts. Caller binds the glow (additive) pipeline first.
    void draw(rhi::CommandList& cmd) const;

    // Drop every live glow (scene switch / session end).
    void clear() { m_glows.clear(); m_quad_count = 0; }

    // Live glows — Renderer reads this to emit one point light per glow.
    std::span<const GlowFx> glow_data() const { return m_glows; }

private:
    std::vector<GlowFx> m_glows;
    u32                 m_next_id = 0;
    rhi::BufferHandle   m_vertex_buffer{};
    u32                 m_quad_count = 0;
    rhi::Rhi*           m_rhi = nullptr;
};

} // namespace uldum::render
