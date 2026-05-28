#include "render/world_overlays.h"

#include "render/gpu_texture.h"
#include "rhi/rhi.h"
#include "asset/asset.h"
#include "asset/texture.h"
#include "core/log.h"

#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "WorldOverlays";

namespace {

constexpr const char* kVertSpv = "engine/shaders/world_overlay.vert.spv";
constexpr const char* kFragSpv = "engine/shaders/world_overlay.frag.spv";

constexpr u32 kMaxVerts  = 16 * 1024;     // per-frame vertex ceiling
constexpr u32 kMaxDraws  = 256;           // per-frame draw ceiling
constexpr f32 kZBias     = 2.0f;          // raise off terrain to avoid z-fight

// Vertex layout — matches world_overlay.vert. RGBA8 is premultiplied
// alpha, packed in one u32 (R in low byte, A in high).
struct Vertex {
    f32 x, y, z;
    f32 u, v;
    u32 rgba;
};
static_assert(sizeof(Vertex) == 24);

struct PushConstants {
    glm::mat4 mvp;
    glm::vec4 tint;
};

struct DrawCmd {
    u32                      first_vertex;
    u32                      vertex_count;
    rhi::DescriptorSetHandle desc_set;
    glm::vec4                tint;
};

u32 pack_premul_rgba(f32 r, f32 g, f32 b, f32 a) {
    if (r < 0) r = 0; else if (r > 1) r = 1;
    if (g < 0) g = 0; else if (g > 1) g = 1;
    if (b < 0) b = 0; else if (b > 1) b = 1;
    if (a < 0) a = 0; else if (a > 1) a = 1;
    u32 R = static_cast<u32>(r * a * 255.0f + 0.5f);
    u32 G = static_cast<u32>(g * a * 255.0f + 0.5f);
    u32 B = static_cast<u32>(b * a * 255.0f + 0.5f);
    u32 A = static_cast<u32>(a       * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}

} // namespace

struct WorldOverlays::Impl {
    rhi::Rhi* rhi = nullptr;

    rhi::PipelineLayoutHandle      pipeline_layout{};
    rhi::PipelineHandle            pipeline{};
    rhi::DescriptorSetLayoutHandle desc_layout{};

    // One default texture per TextureId — generated procedurally at
    // init. Future: expose a setter so a map's hud.json can override
    // by replacing the GpuTexture / desc_set at a slot.
    struct Decal {
        GpuTexture                tex{};
        rhi::DescriptorSetHandle  set{};
    };
    std::array<Decal, static_cast<usize>(WorldOverlays::TextureId::kCount)> decals{};

    // Per-frame mapped VBO; written each frame, drawn once. `mapped` is
    // cached from the RHI's persistent map at create time so per-vertex
    // writes don't pay for a record-table lookup on every call.
    struct Frame {
        rhi::BufferHandle vb{};
        Vertex*           mapped = nullptr;
    };
    std::array<Frame, rhi::MAX_FRAMES_IN_FLIGHT> frames{};

    u32                  next_vertex = 0;
    std::vector<DrawCmd> cmds;
};

// ── Pipeline ─────────────────────────────────────────────────────────────

static rhi::ShaderModuleHandle load_shader(rhi::Rhi& rhi, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return {};
#if defined(ULDUM_BACKEND_GLES)
    std::string resolved(path);
    if (resolved.size() >= 4 && resolved.substr(resolved.size() - 4) == ".spv") {
        resolved.replace(resolved.size() - 4, 4, ".glsl");
    }
    auto bytes = mgr->read_file_bytes(resolved);
#else
    auto bytes = mgr->read_file_bytes(path);
#endif
    if (bytes.empty()) { log::error(TAG, "shader not found: '{}'", path); return {}; }
    return rhi.create_shader_module(bytes);
}


static bool create_descriptor_layout(WorldOverlays::Impl& s) {
    rhi::DescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.type    = rhi::DescriptorType::CombinedImageSampler;
    b.count   = 1;
    b.stages  = rhi::ShaderStage::Fragment;
    rhi::DescriptorSetLayoutDesc desc{};
    desc.bindings = std::span{&b, 1};
    s.desc_layout = s.rhi->create_descriptor_set_layout(desc);
    return s.desc_layout.is_valid();
}

static bool create_pipeline(WorldOverlays::Impl& s) {
    auto vert_h = load_shader(*s.rhi, kVertSpv);
    auto frag_h = load_shader(*s.rhi, kFragSpv);
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        s.rhi->destroy_shader_module(vert_h);
        s.rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(PushConstants);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{&s.desc_layout, 1};
    pl_desc.push_constants = std::span{&pc, 1};
    s.pipeline_layout = s.rhi->create_pipeline_layout(pl_desc);

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(Vertex), false };
    rhi::VertexAttributeDesc attrs[3]{
        { 0, 0, offsetof(Vertex, x),    rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(Vertex, u),    rhi::TextureFormat::R32G32_SFLOAT },
        { 2, 0, offsetof(Vertex, rgba), rhi::TextureFormat::R8G8B8A8_UNORM },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 3};

    rhi::RasterizerState rs{};
    rs.cull_mode = rhi::CullMode::None;

    // Same depth setup as the previous ground-decal pipelines: test
    // against scene depth so 3D geometry occludes the indicator, but
    // no write so subsequent draws aren't affected.
    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = false;
    ds.depth_compare      = rhi::CompareOp::LessEqual;

    rhi::BlendAttachmentState ba{};
    ba.blend_enable     = true;
    ba.src_color_factor = rhi::BlendFactor::One;
    ba.dst_color_factor = rhi::BlendFactor::OneMinusSrcAlpha;
    ba.src_alpha_factor = rhi::BlendFactor::One;
    ba.dst_alpha_factor = rhi::BlendFactor::OneMinusSrcAlpha;

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(s.rhi->msaa_samples());

    rhi::TextureFormat color_fmt = s.rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = s.rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = s.pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.topology          = rhi::PrimitiveTopology::TriangleList;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    s.pipeline = s.rhi->create_graphics_pipeline(desc);

    s.rhi->destroy_shader_module(vert_h);
    s.rhi->destroy_shader_module(frag_h);
    return s.pipeline.is_valid();
}

static bool create_buffers(WorldOverlays::Impl& s) {
    u64 bytes = static_cast<u64>(kMaxVerts) * sizeof(Vertex);
    for (auto& f : s.frames) {
        rhi::BufferDesc d{};
        d.size   = bytes;
        d.usage  = rhi::BufferUsage::Vertex;
        d.memory = rhi::MemoryUsage::HostSequential;
        f.vb = s.rhi->create_buffer(d);
        if (!f.vb.is_valid()) return false;
        f.mapped = static_cast<Vertex*>(s.rhi->mapped_ptr(f.vb));
        if (!f.mapped) return false;
    }
    return true;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

WorldOverlays::WorldOverlays() = default;
WorldOverlays::~WorldOverlays() { shutdown(); }

bool WorldOverlays::init(rhi::Rhi& rhi) {
    m_impl = new Impl{};
    m_impl->rhi = &rhi;
    if (!create_descriptor_layout(*m_impl)) { log::error(TAG, "desc layout failed"); return false; }
    if (!create_pipeline(*m_impl))          { log::error(TAG, "pipeline failed");    return false; }
    if (!create_buffers(*m_impl))           { log::error(TAG, "buffers failed");     return false; }
    // No engine-side default decals. Each slot is unbound until a
    // map's hud.json calls set_texture(); add_* calls referencing an
    // unbound slot are silently dropped. The engine's own
    // default-texture / map-override merging mechanism will fill the
    // gap once it lands.
    log::info(TAG, "world overlays initialized");
    return true;
}

void WorldOverlays::reset_session_state() {
    if (!m_impl || !m_impl->rhi) return;
    // Wait for any in-flight frame to stop sampling these images
    // before we destroy them. Called rarely (once per session end),
    // so the stall is acceptable.
    m_impl->rhi->wait_idle();
    for (auto& d : m_impl->decals) {
        m_impl->rhi->free_descriptor_set(d.set);
        d.set = {};
        destroy_texture(*m_impl->rhi, d.tex);
        d.tex = {};
    }
    m_impl->next_vertex = 0;
    m_impl->cmds.clear();
}

void WorldOverlays::shutdown() {
    if (!m_impl) return;
    if (m_impl->rhi) {
        m_impl->rhi->wait_idle();
        for (auto& d : m_impl->decals) {
            m_impl->rhi->free_descriptor_set(d.set);
            destroy_texture(*m_impl->rhi, d.tex);
        }
        for (auto& f : m_impl->frames) {
            m_impl->rhi->destroy_buffer(f.vb);
        }
        m_impl->rhi->destroy_pipeline(m_impl->pipeline);
        m_impl->rhi->destroy_pipeline_layout(m_impl->pipeline_layout);
        m_impl->rhi->destroy_descriptor_set_layout(m_impl->desc_layout);
    }
    delete m_impl;
    m_impl = nullptr;
}

// ── Geometry helpers ──────────────────────────────────────────────────────

namespace {

u32 append(WorldOverlays::Impl& s, u32 count) {
    if (s.next_vertex + count > kMaxVerts) return UINT32_MAX;
    u32 first = s.next_vertex;
    s.next_vertex += count;
    return first;
}

void write_v(WorldOverlays::Impl& s, u32 slot, u32 i,
             glm::vec3 p, f32 u, f32 v, u32 rgba) {
    s.frames[slot].mapped[i] = { p.x, p.y, p.z + kZBias, u, v, rgba };
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────

bool WorldOverlays::set_texture(TextureId id, std::string_view path) {
    if (!m_impl || !m_impl->rhi || path.empty()) return false;
    if (static_cast<usize>(id) >= m_impl->decals.size()) return false;

    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return false;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::warn(TAG, "set_texture: '{}' not found", path);
        return false;
    }
    auto decoded = asset::load_texture_from_memory(bytes.data(),
                                                   static_cast<u32>(bytes.size()));
    if (!decoded) {
        log::warn(TAG, "set_texture: decode '{}' failed: {}", path, decoded.error());
        return false;
    }
    if (decoded->channels != 4) {
        log::warn(TAG, "set_texture: '{}' has {} channels, expected 4", path, decoded->channels);
        return false;
    }

    GpuTexture new_tex = upload_texture_rgba(*m_impl->rhi,
                                             decoded->pixels.data(),
                                             decoded->width, decoded->height,
                                             /*srgb=*/false, /*clamp=*/true);
    if (!new_tex.texture.is_valid()) {
        log::warn(TAG, "set_texture: upload of '{}' failed", path);
        return false;
    }

    // Wait for in-flight frames to finish referencing the old image
    // before destroying it. This is rare (called once per session at
    // map load), so the stall is acceptable.
    m_impl->rhi->wait_idle();
    auto& d = m_impl->decals[static_cast<usize>(id)];
    destroy_texture(*m_impl->rhi, d.tex);
    d.tex = new_tex;

    // Allocate the slot's descriptor set on first use; on subsequent
    // calls we just re-point the existing set at the new image.
    if (!d.set.is_valid()) {
        d.set = m_impl->rhi->allocate_descriptor_set(m_impl->desc_layout);
        if (!d.set.is_valid()) {
            log::warn(TAG, "set_texture: descriptor set alloc failed for slot {}",
                      static_cast<u32>(id));
            destroy_texture(*m_impl->rhi, d.tex);
            d.tex = {};
            return false;
        }
    }

    rhi::WriteDescriptor w{};
    w.binding = 0;
    w.type    = rhi::DescriptorType::CombinedImageSampler;
    w.texture = d.tex.texture;
    w.sampler = d.tex.sampler;
    m_impl->rhi->update_descriptor_set(d.set, std::span{&w, 1});

    log::info(TAG, "set_texture[{}] = '{}' ({}x{})",
              static_cast<u32>(id), path, decoded->width, decoded->height);
    return true;
}

void WorldOverlays::begin_frame() {
    if (!m_impl) return;
    m_impl->next_vertex = 0;
    m_impl->cmds.clear();
}

void WorldOverlays::add_ring(glm::vec3 center, f32 radius, f32 thickness,
                             glm::vec4 color, TextureId tex,
                             u32 samples_per_ring) {
    if (!m_impl || !m_impl->pipeline.is_valid() || radius <= 0.0f || thickness <= 0.0f) return;
    if (!m_impl->decals[(usize)tex].set.is_valid()) return;  // unbound slot
    if (samples_per_ring < 8) samples_per_ring = 8;
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = samples_per_ring * 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    f32 r_in  = std::max(0.0f, radius - thickness * 0.5f);
    f32 r_out = radius + thickness * 0.5f;
    constexpr f32 TWO_PI = 6.28318530718f;
    f32 step = TWO_PI / samples_per_ring;
    u32 cursor = first;
    for (u32 i = 0; i < samples_per_ring; ++i) {
        f32 a0 = step * i;
        f32 a1 = step * ((i + 1) % samples_per_ring);
        f32 v0 = (f32)i / (f32)samples_per_ring;
        f32 v1 = (f32)(i + 1) / (f32)samples_per_ring;
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        glm::vec3 in0 { center.x + r_in  * c0, center.y + r_in  * s0, center.z };
        glm::vec3 out0{ center.x + r_out * c0, center.y + r_out * s0, center.z };
        glm::vec3 in1 { center.x + r_in  * c1, center.y + r_in  * s1, center.z };
        glm::vec3 out1{ center.x + r_out * c1, center.y + r_out * s1, center.z };
        // CCW: in0(u=0,v0), out0(u=1,v0), out1(u=1,v1), in1(u=0,v1)
        write_v(*m_impl, slot, cursor++, in0,  0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, out0, 1.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, out1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, in0,  0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, out1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, in1,  0.0f, v1, rgba);
    }
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_path(std::span<const glm::vec3> samples, f32 thickness,
                             glm::vec4 color, TextureId tex) {
    if (!m_impl || !m_impl->pipeline.is_valid() || samples.size() < 2 || thickness <= 0.0f) return;
    if (!m_impl->decals[(usize)tex].set.is_valid()) return;  // unbound slot
    u32 slot = m_impl->rhi->frame_index();
    u32 segs = static_cast<u32>(samples.size()) - 1;
    u32 vert_count = segs * 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    f32 hw = thickness * 0.5f;
    u32 cursor = first;
    for (u32 i = 0; i < segs; ++i) {
        glm::vec3 p0 = samples[i];
        glm::vec3 p1 = samples[i + 1];
        f32 v0 = (f32)i / (f32)segs;
        f32 v1 = (f32)(i + 1) / (f32)segs;
        glm::vec3 d  = p1 - p0;
        d.z = 0;
        f32 len2 = d.x * d.x + d.y * d.y;
        glm::vec3 side{ 0.0f, 0.0f, 0.0f };
        if (len2 >= 1e-4f) {
            f32 inv = 1.0f / std::sqrt(len2);
            side = glm::vec3{ -d.y * inv, d.x * inv, 0.0f };
        }
        glm::vec3 a0 = p0 - side * hw;
        glm::vec3 b0 = p0 + side * hw;
        glm::vec3 a1 = p1 - side * hw;
        glm::vec3 b1 = p1 + side * hw;
        // CCW: a0(u=0,v0), b0(u=1,v0), b1(u=1,v1), a1(u=0,v1)
        write_v(*m_impl, slot, cursor++, a0, 0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, b0, 1.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, b1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, a0, 0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, b1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, a1, 0.0f, v1, rgba);
    }
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_quad(glm::vec3 center, f32 half_extent,
                             glm::vec4 color, TextureId tex) {
    if (!m_impl || !m_impl->pipeline.is_valid() || half_extent <= 0.0f) return;
    if (!m_impl->decals[(usize)tex].set.is_valid()) return;  // unbound slot
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    f32 e = half_extent;
    glm::vec3 a{ center.x - e, center.y - e, center.z };  // u=0,v=0
    glm::vec3 b{ center.x + e, center.y - e, center.z };  // u=1,v=0
    glm::vec3 c{ center.x + e, center.y + e, center.z };  // u=1,v=1
    glm::vec3 d{ center.x - e, center.y + e, center.z };  // u=0,v=1
    u32 cursor = first;
    write_v(*m_impl, slot, cursor++, a, 0.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, b, 1.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, c, 1.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, a, 0.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, c, 1.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, d, 0.0f, 1.0f, rgba);
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_cone(glm::vec3 origin, glm::vec3 dir, f32 half_angle,
                             f32 radius, glm::vec4 color, TextureId tex,
                             u32 segments) {
    if (!m_impl || !m_impl->pipeline.is_valid() || radius <= 0.0f || half_angle <= 0.0f) return;
    if (!m_impl->decals[(usize)tex].set.is_valid()) return;  // unbound slot
    if (segments < 4) segments = 4;
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = segments * 3;  // triangle fan as triangle list
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    // Normalize dir in XY.
    glm::vec3 d2 = dir; d2.z = 0;
    f32 len = std::sqrt(d2.x * d2.x + d2.y * d2.y);
    if (len < 1e-4f) return;
    glm::vec3 fwd = d2 / len;
    glm::vec3 side{ -fwd.y, fwd.x, 0.0f };

    u32 cursor = first;
    for (u32 i = 0; i < segments; ++i) {
        f32 t0 = (f32)i / (f32)segments;
        f32 t1 = (f32)(i + 1) / (f32)segments;
        // Map [0,1] → [-half_angle, +half_angle]
        f32 a0 = (t0 * 2.0f - 1.0f) * half_angle;
        f32 a1 = (t1 * 2.0f - 1.0f) * half_angle;
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        glm::vec3 r0 = origin + (fwd * c0 + side * s0) * radius;
        glm::vec3 r1 = origin + (fwd * c1 + side * s1) * radius;
        // origin is u=0.5, v=0 (apex). Rim points: u=t, v=1.
        write_v(*m_impl, slot, cursor++, origin, 0.5f, 0.0f, rgba);
        write_v(*m_impl, slot, cursor++, r0,     t0,   1.0f, rgba);
        write_v(*m_impl, slot, cursor++, r1,     t1,   1.0f, rgba);
    }
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_pillar(glm::vec3 base, f32 height, f32 width,
                               glm::vec3 camera_pos, glm::vec4 color,
                               TextureId tex) {
    if (!m_impl || !m_impl->pipeline.is_valid() || height <= 0.0f || width <= 0.0f) return;
    if (!m_impl->decals[(usize)tex].set.is_valid()) return;  // unbound slot
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    // Right vector lives in the XY plane, perpendicular to (camera - base)
    // so the broad face stays facing the camera as it orbits. Falls back
    // to world +X when the camera is directly overhead (degenerate).
    glm::vec3 to_cam{ camera_pos.x - base.x, camera_pos.y - base.y, 0.0f };
    f32 len2 = to_cam.x * to_cam.x + to_cam.y * to_cam.y;
    glm::vec3 right{ 1.0f, 0.0f, 0.0f };
    if (len2 >= 1e-6f) {
        f32 inv = 1.0f / std::sqrt(len2);
        right = glm::vec3{ -to_cam.y * inv, to_cam.x * inv, 0.0f };
    }
    glm::vec3 r = right * (width * 0.5f);
    glm::vec3 up{ 0.0f, 0.0f, height };
    // CCW from camera POV. UV V=0 at top, V=1 at base — gradient or
    // any other look lives in the texture.
    glm::vec3 bl = base - r;
    glm::vec3 br = base + r;
    glm::vec3 tr = base + r + up;
    glm::vec3 tl = base - r + up;
    u32 cursor = first;
    write_v(*m_impl, slot, cursor++, bl, 0.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, br, 1.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, tr, 1.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, bl, 0.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, tr, 1.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, tl, 0.0f, 0.0f, rgba);
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

// ── Per-frame draw ────────────────────────────────────────────────────────

void WorldOverlays::draw(rhi::CommandList& cmd, const glm::mat4& view_projection) {
    if (!m_impl || !m_impl->pipeline.is_valid() || m_impl->cmds.empty()) return;
    u32 slot = m_impl->rhi->frame_index();
    auto& f = m_impl->frames[slot];
    if (!f.vb.is_valid()) return;

    cmd.bind_pipeline(m_impl->pipeline);
    rhi::Extent2D ex = m_impl->rhi->extent();
    cmd.set_viewport(0, 0, static_cast<f32>(ex.width), static_cast<f32>(ex.height));
    cmd.set_scissor(0, 0, ex.width, ex.height);
    cmd.bind_vertex_buffer(0, f.vb);

    PushConstants pc{};
    pc.mvp = view_projection;
    rhi::DescriptorSetHandle last_set{};
    for (const auto& d : m_impl->cmds) {
        if (d.desc_set != last_set) {
            cmd.bind_descriptor_set(m_impl->pipeline_layout, 0, d.desc_set);
            last_set = d.desc_set;
        }
        pc.tint = d.tint;
        cmd.push_constants(m_impl->pipeline_layout, rhi::ShaderStage::Vertex,
                           0, sizeof(PushConstants), &pc);
        cmd.draw(d.vertex_count, 1, d.first_vertex, 0);
    }
}

} // namespace uldum::render
