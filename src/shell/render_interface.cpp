#include "shell/render_interface.h"
#include "rhi/rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace uldum::shell {

static constexpr const char* TAG = "UI";

// Keep in sync with Rml::Vertex (position f32×2, colour u8×4 premul, tex_coord f32×2).
static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex layout changed");

// ── Shader loading helper (mirrors the pattern in renderer.cpp) ──────────
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
    if (bytes.empty()) {
        log::error(TAG, "UI shader not found: '{}'", path);
        return {};
    }
    return rhi.create_shader_module(bytes);
}

RenderInterface::RenderInterface(rhi::Rhi& rhi) : m_rhi(rhi) {}
RenderInterface::~RenderInterface() { shutdown(); }

bool RenderInterface::init() {
    if (!create_descriptor_layout()) { log::error(TAG, "desc layout create failed"); return false; }
    if (!create_descriptor_pool())   { log::error(TAG, "desc pool create failed"); return false; }
    if (!create_sampler())           { log::error(TAG, "sampler create failed"); return false; }
    if (!create_pipeline_layout())   { log::error(TAG, "pipe layout create failed"); return false; }
    if (!create_pipeline())          { log::error(TAG, "pipeline create failed"); return false; }
    if (!create_white_texture())     { log::error(TAG, "white texture create failed"); return false; }
    return true;
}

void RenderInterface::shutdown() {
    m_rhi.wait_idle();

    for (auto& [h, g] : m_geometries) destroy_geometry(g);
    m_geometries.clear();
    for (auto& [h, t] : m_textures)   destroy_texture(t);
    m_textures.clear();
    destroy_texture(m_white);

    m_rhi.destroy_pipeline(m_pipeline);
    m_rhi.destroy_pipeline_layout(m_pipeline_layout);
    m_rhi.destroy_sampler(m_sampler);
    m_rhi.destroy_descriptor_set_layout(m_desc_layout);
}

// ── One-time setup ───────────────────────────────────────────────────────

bool RenderInterface::create_descriptor_layout() {
    rhi::DescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.type    = rhi::DescriptorType::CombinedImageSampler;
    b.count   = 1;
    b.stages  = rhi::ShaderStage::Fragment;
    rhi::DescriptorSetLayoutDesc desc{};
    desc.bindings = std::span{&b, 1};
    m_desc_layout = m_rhi.create_descriptor_set_layout(desc);
    return m_desc_layout.is_valid();
}

bool RenderInterface::create_descriptor_pool() {
    // Pools are now hidden inside the RHI; nothing to do here. Kept as a
    // no-op so the init order in `init()` doesn't need to change.
    return true;
}

bool RenderInterface::create_sampler() {
    rhi::SamplerDesc sd{};
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    sd.address_w = rhi::AddressMode::ClampToEdge;
    m_sampler = m_rhi.create_sampler(sd);
    return m_sampler.is_valid();
}

bool RenderInterface::create_pipeline_layout() {
    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(glm::mat4);
    rhi::PipelineLayoutDesc d{};
    d.set_layouts    = std::span{&m_desc_layout, 1};
    d.push_constants = std::span{&pc, 1};
    m_pipeline_layout = m_rhi.create_pipeline_layout(d);
    return m_pipeline_layout.is_valid();
}

bool RenderInterface::create_pipeline() {
    auto vert_h = load_shader(m_rhi, "engine/shaders/ui_shell.vert.spv");
    auto frag_h = load_shader(m_rhi, "engine/shaders/ui_shell.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        m_rhi.destroy_shader_module(vert_h);
        m_rhi.destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(Rml::Vertex), false };
    rhi::VertexAttributeDesc attrs[3]{
        { 0, 0, offsetof(Rml::Vertex, position),  rhi::TextureFormat::R32G32_SFLOAT  },
        { 1, 0, offsetof(Rml::Vertex, colour),    rhi::TextureFormat::R8G8B8A8_UNORM },
        { 2, 0, offsetof(Rml::Vertex, tex_coord), rhi::TextureFormat::R32G32_SFLOAT  },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 3};

    rhi::RasterizerState rs{};
    rs.cull_mode = rhi::CullMode::None;

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = false;
    ds.depth_write_enable = false;

    rhi::BlendAttachmentState ba{};
    ba.blend_enable     = true;
    ba.src_color_factor = rhi::BlendFactor::One;
    ba.dst_color_factor = rhi::BlendFactor::OneMinusSrcAlpha;
    ba.src_alpha_factor = rhi::BlendFactor::One;
    ba.dst_alpha_factor = rhi::BlendFactor::OneMinusSrcAlpha;

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi.msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi.swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi.depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.topology          = rhi::PrimitiveTopology::TriangleList;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_pipeline = m_rhi.create_graphics_pipeline(desc);
    m_rhi.destroy_shader_module(vert_h);
    m_rhi.destroy_shader_module(frag_h);
    if (!m_pipeline.is_valid()) {
        log::error(TAG, "create_graphics_pipeline (UI) failed");
        return false;
    }
    return true;
}

// ── Texture helpers ──────────────────────────────────────────────────────

rhi::DescriptorSetHandle RenderInterface::allocate_texture_set(rhi::TextureHandle tex) {
    auto set = m_rhi.allocate_descriptor_set(m_desc_layout);
    if (!set.is_valid()) {
        log::error(TAG, "UI descriptor allocation failed");
        return {};
    }
    rhi::WriteDescriptor w{};
    w.binding = 0;
    w.type    = rhi::DescriptorType::CombinedImageSampler;
    w.texture = tex;
    w.sampler = m_sampler;
    m_rhi.update_descriptor_set(set, std::span{&w, 1});
    return set;
}

RenderInterface::Texture RenderInterface::create_texture_from_rgba(const u8* rgba, u32 w, u32 h) {
    Texture tex{};
    u64 size = static_cast<u64>(w) * h * 4;

    rhi::BufferDesc bd{};
    bd.size   = size;
    bd.usage  = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::HostSequential;
    auto stage = m_rhi.create_buffer(bd);
    if (!stage.is_valid()) {
        log::error(TAG, "staging buffer create failed");
        return tex;
    }
    void* stage_mapped = m_rhi.mapped_ptr(stage);
    if (!stage_mapped) {
        log::error(TAG, "staging buffer not mapped");
        m_rhi.destroy_buffer(stage);
        return tex;
    }
    std::memcpy(stage_mapped, rgba, size);

    {
        rhi::TextureDesc td{};
        td.width  = w;
        td.height = h;
        td.format = rhi::TextureFormat::R8G8B8A8_UNORM;
        td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        tex.handle = m_rhi.create_texture(td);
        if (!tex.handle.is_valid()) {
            log::error(TAG, "image create failed");
            m_rhi.destroy_buffer(stage);
            return tex;
        }
    }

    // Upload via one-shot command buffer: UNDEFINED → TRANSFER_DST → copy → SHADER_READ_ONLY.
    rhi::CommandList cmd = m_rhi.begin_oneshot();
    {
        rhi::ImageBarrier b{};
        b.image      = tex.handle;
        b.src_stage  = rhi::PipelineStage::TopOfPipe;
        b.dst_stage  = rhi::PipelineStage::Transfer;
        b.dst_access = rhi::AccessFlag::TransferWrite;
        b.old_layout = rhi::ImageLayout::Undefined;
        b.new_layout = rhi::ImageLayout::TransferDstOptimal;
        cmd.image_barrier(b);

        rhi::BufferImageCopy copy{};
        copy.image_extent_w = w;
        copy.image_extent_h = h;
        cmd.copy_buffer_to_image(stage, tex.handle, std::span{&copy, 1});

        rhi::ImageBarrier b2{};
        b2.image      = tex.handle;
        b2.src_stage  = rhi::PipelineStage::Transfer;
        b2.src_access = rhi::AccessFlag::TransferWrite;
        b2.dst_stage  = rhi::PipelineStage::FragmentShader;
        b2.dst_access = rhi::AccessFlag::ShaderRead;
        b2.old_layout = rhi::ImageLayout::TransferDstOptimal;
        b2.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        cmd.image_barrier(b2);
    }
    m_rhi.end_oneshot(cmd);

    m_rhi.destroy_buffer(stage);

    tex.set = allocate_texture_set(tex.handle);
    return tex;
}

void RenderInterface::destroy_texture(Texture& tex) {
    m_rhi.free_descriptor_set(tex.set);
    tex.set = {};
    m_rhi.destroy_texture(tex.handle);
}

bool RenderInterface::create_white_texture() {
    u8 pixel[4] = {255, 255, 255, 255};
    m_white = create_texture_from_rgba(pixel, 1, 1);
    return m_white.set.is_valid();
}

// ── Geometry ─────────────────────────────────────────────────────────────

void RenderInterface::destroy_geometry(Geometry& g) {
    m_rhi.destroy_buffer(g.vb);
    m_rhi.destroy_buffer(g.ib);
    g = {};
}

Rml::CompiledGeometryHandle RenderInterface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    Geometry g{};
    g.index_count = static_cast<u32>(indices.size());

    auto make = [&](rhi::BufferUsage usage, u64 size, const void* src,
                    rhi::BufferHandle& out) -> bool {
        rhi::BufferDesc d{};
        d.size   = size;
        d.usage  = usage;
        d.memory = rhi::MemoryUsage::HostSequential;
        out = m_rhi.create_buffer(d);
        if (!out.is_valid()) return false;
        if (void* dst = m_rhi.mapped_ptr(out)) {
            std::memcpy(dst, src, size);
            return true;
        }
        return false;
    };

    u64 vb_size = vertices.size() * sizeof(Rml::Vertex);
    u64 ib_size = indices.size()  * sizeof(u32);
    if (!make(rhi::BufferUsage::Vertex, vb_size, vertices.data(), g.vb)) return 0;

    // RmlUi indices are `int`; Vulkan wants u32. Same size, trivially copy.
    if (!make(rhi::BufferUsage::Index, ib_size, indices.data(), g.ib)) {
        m_rhi.destroy_buffer(g.vb);
        return 0;
    }

    auto handle = m_next_geom++;
    m_geometries.emplace(handle, g);
    return handle;
}

void RenderInterface::ensure_pipeline_bound() {
    if (m_pipeline_bound || !m_cmd) return;
    m_pipeline_bound = true;
    m_cmd->bind_pipeline(m_pipeline);
    m_cmd->set_viewport(0, 0, static_cast<f32>(m_extent.width), static_cast<f32>(m_extent.height));

    // Default scissor covers full viewport until SetScissorRegion overrides.
    m_scissor         = { 0, 0, m_extent.width, m_extent.height };
    m_scissor_enabled = false;
    m_cmd->set_scissor(0, 0, m_extent.width, m_extent.height);
}

void RenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                     Rml::Vector2f translation,
                                     Rml::TextureHandle texture)
{
    auto it = m_geometries.find(geometry);
    if (it == m_geometries.end() || !m_cmd) return;
    auto& g = it->second;

    ensure_pipeline_bound();

    // MVP = ortho * transform * translate(translation).
    // glm::ortho is GL convention (+Y up). On Vulkan that lines up with
    // Vulkan's +Y-down NDC so RmlUi's top-left-origin coords land right
    // side up. On GLES, swap top/bottom to get the same screen-to-NDC
    // mapping.
#if defined(ULDUM_BACKEND_GLES)
    glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(m_extent.width),
                                 static_cast<float>(m_extent.height), 0.0f,
                                 -1.0f, 1.0f);
#else
    glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(m_extent.width),
                                 0.0f, static_cast<float>(m_extent.height),
                                 -1.0f, 1.0f);
#endif
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(translation.x, translation.y, 0.0f));
    glm::mat4 mvp = ortho * m_transform * t;
    m_cmd->push_constants(m_pipeline_layout, rhi::ShaderStage::Vertex,
                          0, sizeof(glm::mat4), glm::value_ptr(mvp));

    // Apply scissor (track state to avoid rebinding when unchanged).
    m_cmd->set_scissor(m_scissor.x, m_scissor.y, m_scissor.w, m_scissor.h);

    rhi::DescriptorSetHandle tex_set = m_white.set;
    if (texture != 0) {
        auto t_it = m_textures.find(texture);
        if (t_it != m_textures.end() && t_it->second.set.is_valid()) tex_set = t_it->second.set;
    }
    m_cmd->bind_descriptor_set(m_pipeline_layout, 0, tex_set);
    m_cmd->bind_vertex_buffer(0, g.vb);
    m_cmd->bind_index_buffer(g.ib, 0, rhi::IndexType::U32);
    m_cmd->draw_indexed(g.index_count);
}

void RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    auto it = m_geometries.find(geometry);
    if (it == m_geometries.end()) return;
    destroy_geometry(it->second);
    m_geometries.erase(it);
}

// ── Texture virtuals ─────────────────────────────────────────────────────

Rml::TextureHandle RenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                const Rml::String& source)
{
    auto* mgr = asset::AssetManager::instance();
    std::vector<u8> bytes;
    if (mgr) bytes = mgr->read_file_bytes(source.c_str());
    if (bytes.empty()) {
        // Filesystem fallback (mirrors FileInterface so loose files in dist work).
        std::FILE* f = std::fopen(source.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            auto n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            bytes.resize(static_cast<size_t>(n));
            std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    if (bytes.empty()) {
        log::warn(TAG, "LoadTexture: '{}' not found", source.c_str());
        return 0;
    }

    auto decoded = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
    if (!decoded || decoded->channels != 4) {
        log::warn(TAG, "LoadTexture: '{}' decode failed / not RGBA", source.c_str());
        return 0;
    }
    Texture tex = create_texture_from_rgba(decoded->pixels.data(), decoded->width, decoded->height);
    if (!tex.set.is_valid()) return 0;

    texture_dimensions = Rml::Vector2i(static_cast<int>(decoded->width),
                                       static_cast<int>(decoded->height));
    auto handle = m_next_tex++;
    m_textures.emplace(handle, tex);
    return handle;
}

Rml::TextureHandle RenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                    Rml::Vector2i source_dimensions)
{
    // Used for the font glyph atlas. RmlUi passes raw RGBA bytes.
    Texture tex = create_texture_from_rgba(source.data(),
                                           static_cast<u32>(source_dimensions.x),
                                           static_cast<u32>(source_dimensions.y));
    if (!tex.set.is_valid()) return 0;
    auto handle = m_next_tex++;
    m_textures.emplace(handle, tex);
    return handle;
}

void RenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    auto it = m_textures.find(texture);
    if (it == m_textures.end()) return;
    destroy_texture(it->second);
    m_textures.erase(it);
}

// ── Scissor / transform ──────────────────────────────────────────────────

void RenderInterface::EnableScissorRegion(bool enable) {
    m_scissor_enabled = enable;
    if (!enable) m_scissor = { 0, 0, m_extent.width, m_extent.height };
}

void RenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    m_scissor.x = region.Left();
    m_scissor.y = region.Top();
    m_scissor.w = static_cast<u32>(region.Width());
    m_scissor.h = static_cast<u32>(region.Height());
}

void RenderInterface::SetTransform(const Rml::Matrix4f* transform) {
    if (transform) {
        // Rml::Matrix4f is column-major in RmlUi's default config. Reinterpret
        // as a glm::mat4 (also column-major). If RmlUi is built with
        // ROW_MAJOR_MATRICES we'd need to transpose; we build default.
        std::memcpy(&m_transform, transform, sizeof(glm::mat4));
    } else {
        m_transform = glm::mat4(1.0f);
    }
}

// ── Frame setup ──────────────────────────────────────────────────────────

void RenderInterface::begin_frame(rhi::CommandList& cmd, rhi::Extent2D extent) {
    m_cmd              = &cmd;
    m_extent           = extent;
    m_pipeline_bound   = false;
    m_scissor_enabled  = false;
    m_scissor          = { 0, 0, extent.width, extent.height };
    m_transform        = glm::mat4(1.0f);
}

} // namespace uldum::shell
