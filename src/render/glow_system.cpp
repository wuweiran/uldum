#include "render/glow_system.h"
#include "rhi/rhi.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace uldum::render {

static constexpr const char* TAG = "Glow";

bool GlowSystem::init(rhi::Rhi& rhi) {
    m_rhi = &rhi;
    rhi::BufferDesc d{};
    // One shaft (1 quad = 6 verts) per glow.
    d.size   = static_cast<u64>(MAX_GLOWS) * 6 * sizeof(GlowVertex);
    d.usage  = rhi::BufferUsage::Vertex;
    d.memory = rhi::MemoryUsage::HostSequential;
    m_vertex_buffer = rhi.create_buffer(d);
    if (!m_vertex_buffer.is_valid()) {
        log::error(TAG, "Failed to create glow vertex buffer");
        return false;
    }
    m_glows.reserve(32);
    log::info(TAG, "Glow system initialized (max {} glows)", MAX_GLOWS);
    return true;
}

void GlowSystem::shutdown() {
    if (m_rhi) m_rhi->destroy_buffer(m_vertex_buffer);
    m_vertex_buffer = {};
    m_glows.clear();
    m_rhi = nullptr;
}

u32 GlowSystem::spawn(glm::vec3 base, const GlowParams& params, glm::vec4 color) {
    if (m_glows.size() >= MAX_GLOWS) return 0;
    GlowFx g;
    g.id     = ++m_next_id;
    g.base   = base;
    g.params = params;
    g.color  = color;
    g.age    = 0;
    m_glows.push_back(std::move(g));
    return m_glows.back().id;
}

bool GlowSystem::set_base(u32 id, glm::vec3 base) {
    if (id == 0) return false;
    for (auto& g : m_glows) {
        if (g.id == id) { g.base = base; return true; }
    }
    return false;  // faded out and erased
}

void GlowSystem::update(f32 dt) {
    for (auto& g : m_glows) g.age += dt;
    std::erase_if(m_glows, [](const GlowFx& g) { return g.age >= g.params.life; });
}

void GlowSystem::upload(glm::vec3 camera_pos) {
    m_quad_count = 0;
    if (m_glows.empty()) return;
    auto* verts = static_cast<GlowVertex*>(m_rhi->mapped_ptr(m_vertex_buffer));
    if (!verts) return;

    u32 q = 0;
    for (const auto& g : m_glows) {
        if (q >= MAX_GLOWS) break;

        // Brightness envelope: linear ramp up over fade_in, hold, then linear
        // ramp down over fade_out (both in seconds). Unset (0) defaults to half
        // of life each — the old symmetric triangle. The shader reads `fade` to
        // retract the Tyndall halo toward the core as it dies, so a low value
        // reads as light weakening, not just a flat alpha dim.
        f32 life     = g.params.life;
        f32 fade_in  = (g.params.fade_in  > 0) ? g.params.fade_in  : life * 0.5f;
        f32 fade_out = (g.params.fade_out > 0) ? g.params.fade_out : life * 0.5f;
        // If the two ramps overlap (sum > life), scale them to fit so the peak
        // lands where they meet instead of clipping.
        if (fade_in + fade_out > life && fade_in + fade_out > 0) {
            f32 s = life / (fade_in + fade_out);
            fade_in *= s;
            fade_out *= s;
        }
        f32 fade;
        if (g.age < fade_in) {
            fade = (fade_in > 0) ? (g.age / fade_in) : 1.0f;
        } else if (g.age > life - fade_out) {
            fade = (fade_out > 0) ? ((life - g.age) / fade_out) : 1.0f;
        } else {
            fade = 1.0f;
        }
        fade = std::clamp(fade, 0.0f, 1.0f);
        glm::vec4 color = g.color;
        color.a *= fade * g.params.intensity;          // baked brightness

        f32 height  = g.params.height;
        f32 width   = g.params.radius;                 // beam width
        f32 tyndall = g.params.tyndall;

        // Static anchor — the shaft does not rise; it just fades.
        glm::vec3 anchor = g.base;

        // Camera-yaw-aligned right vector: perpendicular to the anchor→camera
        // vector in the XY plane, so the broad face always faces the camera.
        glm::vec3 to_cam{camera_pos.x - anchor.x, camera_pos.y - anchor.y, 0.0f};
        glm::vec3 right;
        if (glm::dot(to_cam, to_cam) < 1e-5f) {
            right = {1.0f, 0.0f, 0.0f};  // camera overhead — degenerate
        } else {
            to_cam = glm::normalize(to_cam);
            right  = glm::normalize(glm::vec3{-to_cam.y, to_cam.x, 0.0f});
        }
        glm::vec3 half = right * (width * 0.5f);
        glm::vec3 up   = glm::vec3{0.0f, 0.0f, height};

        glm::vec3 bl = anchor - half;            // base-left
        glm::vec3 brr= anchor + half;            // base-right
        glm::vec3 tr = anchor + half + up;       // top-right
        glm::vec3 tl = anchor - half + up;       // top-left

        // UV: U across width (0..1), V up the shaft (0 base → 1 top).
        u32 base = q * 6;
        verts[base + 0] = {bl,  color, {0.0f, 0.0f}, tyndall, fade};
        verts[base + 1] = {brr, color, {1.0f, 0.0f}, tyndall, fade};
        verts[base + 2] = {tr,  color, {1.0f, 1.0f}, tyndall, fade};
        verts[base + 3] = {bl,  color, {0.0f, 0.0f}, tyndall, fade};
        verts[base + 4] = {tr,  color, {1.0f, 1.0f}, tyndall, fade};
        verts[base + 5] = {tl,  color, {0.0f, 1.0f}, tyndall, fade};
        ++q;
    }
    m_quad_count = q;
}

void GlowSystem::draw(rhi::CommandList& cmd) const {
    if (m_quad_count == 0 || !m_vertex_buffer.is_valid()) return;
    cmd.bind_vertex_buffer(0, m_vertex_buffer);
    cmd.draw(m_quad_count * 6);
}

} // namespace uldum::render
