#include "editor/editor_overlays.h"

#include "render/gpu_texture.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstring>

namespace uldum::editor {

static constexpr const char* TAG = "EditorOverlays";

namespace {

// Reuse the engine's world-overlay shader pair — same vertex layout
// (vec3 pos + vec2 uv + RGBA8) and same push-constant block, so we
// don't ship a second pair of SPIR-V binaries inside engine.uldpak.
// The editor binds a 1x1 white texture to the sampler so the fragment
// shader's `texture * in_color` multiply collapses to in_color.
constexpr const char* kVertSpv = "engine/shaders/world_overlay.vert.spv";
constexpr const char* kFragSpv = "engine/shaders/world_overlay.frag.spv";

constexpr u32 kMaxVerts = 8 * 1024;   // 4096 line segments per frame
constexpr f32 kZBias    = 1.0f;       // small lift so wires don't z-fight terrain

struct Vertex {
    f32 x, y, z;
    f32 u, v;     // unused for lines; kept for shader-layout parity
    u32 rgba;     // premultiplied-alpha RGBA8
};
static_assert(sizeof(Vertex) == 24);

struct PushConstants {
    glm::mat4 mvp;
    glm::vec4 tint;
};

// Premultiplied alpha — matches the world_overlay shader's expectation
// (it produces premultiplied output for premultiplied-alpha blend).
u32 pack_premul_rgba(glm::vec4 c) {
    auto clamp = [](f32 v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
    f32 r = clamp(c.r), g = clamp(c.g), b = clamp(c.b), a = clamp(c.a);
    u32 R = static_cast<u32>(r * a * 255.0f + 0.5f);
    u32 G = static_cast<u32>(g * a * 255.0f + 0.5f);
    u32 B = static_cast<u32>(b * a * 255.0f + 0.5f);
    u32 A = static_cast<u32>(a     * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}

} // namespace

struct EditorOverlays::Impl {
    rhi::Rhi* rhi = nullptr;

    rhi::PipelineLayoutHandle      pipeline_layout{};
    rhi::PipelineHandle            pipeline{};
    rhi::DescriptorSetLayoutHandle desc_layout{};
    rhi::DescriptorSetHandle       desc_set{};

    // 1x1 white texture stand-in so the shared fragment shader's
    // sample is identity for line draws.
    render::GpuTexture    white_tex{};

    struct Frame {
        rhi::BufferHandle vb{};
        Vertex*           mapped = nullptr;
    };
    std::array<Frame, rhi::MAX_FRAMES_IN_FLIGHT> frames{};

    u32 next_vertex = 0;
};

// ── Pipeline ─────────────────────────────────────────────────────────────

static rhi::ShaderModuleHandle load_shader(rhi::Rhi& rhi, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return {};
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::error(TAG, "shader not found: '{}'", path);
        return {};
    }
    return rhi.create_shader_module(bytes);
}

static bool create_descriptors(EditorOverlays::Impl& s) {
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

static bool create_white_texture(EditorOverlays::Impl& s) {
    const u8 white[4] = { 255, 255, 255, 255 };
    s.white_tex = render::upload_texture_rgba(*s.rhi, white, 1, 1,
                                              /*srgb=*/false, /*clamp=*/true);
    if (!s.white_tex.texture.is_valid()) return false;

    s.desc_set = s.rhi->allocate_descriptor_set(s.desc_layout);
    if (!s.desc_set.is_valid()) return false;

    rhi::WriteDescriptor w{};
    w.binding = 0;
    w.type    = rhi::DescriptorType::CombinedImageSampler;
    w.texture = s.white_tex.texture;
    w.sampler = s.white_tex.sampler;
    s.rhi->update_descriptor_set(s.desc_set, std::span{&w, 1});
    return true;
}

static bool create_pipeline(EditorOverlays::Impl& s) {
    auto vert_h = load_shader(*s.rhi, kVertSpv);
    auto frag_h = load_shader(*s.rhi, kFragSpv);
    if (!s.rhi->resolve(vert_h) || !s.rhi->resolve(frag_h)) {
        s.rhi->destroy_shader_module(vert_h);
        s.rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.offset = 0;
    pc.size   = sizeof(PushConstants);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{&s.desc_layout, 1};
    pl_desc.push_constants = std::span{&pc, 1};
    s.pipeline_layout = s.rhi->create_pipeline_layout(pl_desc);

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;
    stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment;
    stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{};
    binding.binding = 0;
    binding.stride  = sizeof(Vertex);

    rhi::VertexAttributeDesc attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format   = rhi::TextureFormat::R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, x);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format   = rhi::TextureFormat::R32G32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, u);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format   = rhi::TextureFormat::R8G8B8A8_UNORM;
    attrs[2].offset   = offsetof(Vertex, rgba);

    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 3};

    rhi::RasterizerState rs{};
    rs.cull_mode = rhi::CullMode::None;

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
    desc.topology          = rhi::PrimitiveTopology::LineList;
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

static bool create_buffers(EditorOverlays::Impl& s) {
    VkDeviceSize bytes = static_cast<VkDeviceSize>(kMaxVerts) * sizeof(Vertex);
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

EditorOverlays::EditorOverlays() = default;
EditorOverlays::~EditorOverlays() { shutdown(); }

bool EditorOverlays::init(rhi::Rhi& rhi) {
    m_impl = new Impl{};
    m_impl->rhi = &rhi;
    if (!create_descriptors(*m_impl))    { log::error(TAG, "descriptors failed"); return false; }
    if (!create_pipeline(*m_impl))       { log::error(TAG, "pipeline failed");    return false; }
    if (!create_buffers(*m_impl))        { log::error(TAG, "buffers failed");     return false; }
    if (!create_white_texture(*m_impl))  { log::error(TAG, "white tex failed");   return false; }
    log::info(TAG, "editor overlays initialized");
    return true;
}

void EditorOverlays::shutdown() {
    if (!m_impl) return;
    VkDevice device = m_impl->rhi ? m_impl->rhi->device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        render::destroy_texture(*m_impl->rhi, m_impl->white_tex);
        for (auto& f : m_impl->frames) {
            m_impl->rhi->destroy_buffer(f.vb);
        }
        m_impl->rhi->destroy_pipeline(m_impl->pipeline);
        m_impl->rhi->destroy_pipeline_layout(m_impl->pipeline_layout);
        m_impl->rhi->free_descriptor_set(m_impl->desc_set);
        m_impl->rhi->destroy_descriptor_set_layout(m_impl->desc_layout);
    }
    delete m_impl;
    m_impl = nullptr;
}

// ── API ──────────────────────────────────────────────────────────────────

void EditorOverlays::begin_frame() {
    if (!m_impl) return;
    m_impl->next_vertex = 0;
}

void EditorOverlays::add_line(glm::vec3 a, glm::vec3 b, glm::vec4 color) {
    if (!m_impl || !m_impl->pipeline.is_valid()) return;
    if (m_impl->next_vertex + 2 > kMaxVerts) return;
    Vertex* mapped = m_impl->frames[m_impl->rhi->frame_index()].mapped;
    u32 rgba = pack_premul_rgba(color);
    mapped[m_impl->next_vertex++] = { a.x, a.y, a.z + kZBias, 0.0f, 0.0f, rgba };
    mapped[m_impl->next_vertex++] = { b.x, b.y, b.z + kZBias, 0.0f, 0.0f, rgba };
}

void EditorOverlays::add_polyline(std::span<const glm::vec3> samples,
                                  glm::vec4 color, bool closed) {
    if (!m_impl || !m_impl->pipeline.is_valid() || samples.size() < 2) return;
    u32 segs = static_cast<u32>(samples.size() - 1) + (closed ? 1u : 0u);
    if (m_impl->next_vertex + segs * 2 > kMaxVerts) return;
    Vertex* mapped = m_impl->frames[m_impl->rhi->frame_index()].mapped;
    u32 rgba = pack_premul_rgba(color);
    u32 cursor = m_impl->next_vertex;
    auto write = [&](glm::vec3 p) {
        mapped[cursor++] = { p.x, p.y, p.z + kZBias, 0.0f, 0.0f, rgba };
    };
    for (usize i = 0; i + 1 < samples.size(); ++i) {
        write(samples[i]);
        write(samples[i + 1]);
    }
    if (closed) {
        write(samples.back());
        write(samples.front());
    }
    m_impl->next_vertex = cursor;
}

// ── Draw ─────────────────────────────────────────────────────────────────

void EditorOverlays::draw(rhi::CommandList& cmd, const glm::mat4& view_projection) {
    if (!m_impl || !m_impl->pipeline.is_valid() || m_impl->next_vertex == 0) return;
    u32 slot = m_impl->rhi->frame_index();
    auto& f = m_impl->frames[slot];
    if (!f.vb.is_valid()) return;

    cmd.bind_pipeline(m_impl->pipeline);
    rhi::Extent2D ex = m_impl->rhi->extent();
    cmd.set_viewport(0, 0, static_cast<f32>(ex.width), static_cast<f32>(ex.height));
    cmd.set_scissor(0, 0, ex.width, ex.height);
    cmd.bind_descriptor_set(m_impl->pipeline_layout, 0, m_impl->desc_set);
    cmd.bind_vertex_buffer(0, f.vb);

    PushConstants pc{};
    pc.mvp  = view_projection;
    pc.tint = glm::vec4(1.0f);
    cmd.push_constants(m_impl->pipeline_layout, rhi::ShaderStage::Vertex,
                       0, sizeof(PushConstants), &pc);
    cmd.draw(m_impl->next_vertex);
}

} // namespace uldum::editor
