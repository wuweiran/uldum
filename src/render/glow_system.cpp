#include "render/glow_system.h"
#include "rhi/rhi.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace uldum::render {

static constexpr const char* TAG = "Glow";

f32 glow_envelope(f32 age, const GlowParams& params) {
    f32 life = params.life;
    if (life <= 0.0f) return 0.0f;
    f32 fade_in  = (params.fade_in  > 0) ? params.fade_in  : life * 0.10f;
    f32 fade_out = (params.fade_out > 0) ? params.fade_out : life * 0.20f;
    // Scale overlapping ramps to fit so the peak lands where they meet.
    if (fade_in + fade_out > life) {
        f32 s = life / (fade_in + fade_out);
        fade_in *= s;
        fade_out *= s;
    }
    f32 e;
    if (age < fade_in) {
        e = (fade_in > 0) ? (age / fade_in) : 1.0f;
    } else if (age > life - fade_out) {
        e = (fade_out > 0) ? ((life - age) / fade_out) : 0.0f;
    } else {
        e = 1.0f;
    }
    e = std::clamp(e, 0.0f, 1.0f);
    return e * e * (3.0f - 2.0f * e);   // smoothstep ease
}

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

        f32 fade = glow_envelope(g.age, g.params);
        glm::vec4 color = g.color;
        // Bake the static brightness cap; the envelope rides separately as `fade`.
        color.a *= g.params.intensity;

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
