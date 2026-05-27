#include "render/hud/hud_renderer.h"
#include "render/hud/world.h"
#include "render/hud/font.h"

#include "hud/hud_impl.h"
#include "hud/node.h"
#include "hud/text_tag.h"

#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "asset/texture.h"
#include "simulation/world.h"
#include "simulation/components.h"
#include "simulation/ability_def.h"
#include "simulation/simulation.h"
#include "simulation/type_registry.h"
#include "simulation/vision.h"
#include "map/terrain_data.h"
#include "simulation/selection.h"
#include "render/camera.h"
#include "core/log.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uldum::hud {

static constexpr const char* TAG = "HudRenderer";

// ── Render-side primitives ───────────────────────────────────────────────
// Vulkan / VMA types and the CPU-side staging buffers used by the HUD's
// quad batcher. Kept private to this translation unit so the data lib
// (uldum_hud) doesn't see any of it.

constexpr u32 MAX_QUADS = 4096;
constexpr u32 MAX_VERTS = MAX_QUADS * 4;
constexpr u32 MAX_INDS  = MAX_QUADS * 6;

struct Vertex {
    f32 pos[2];
    u32 color;   // RGBA8, matches VK_FORMAT_R8G8B8A8_UNORM
    f32 uv[2];
};
static_assert(sizeof(Vertex) == 20, "HUD Vertex layout must stay tightly packed");

struct RingBuffer {
    rhi::BufferHandle vb{};
    rhi::BufferHandle ib{};
};

struct HudImage {
    rhi::TextureHandle       handle{};
    rhi::DescriptorSetHandle set{};
    u32 w = 0;
    u32 h = 0;
};

enum PipelineKind : u32 { PIPE_SOLID = 0, PIPE_TEXT = 1 };

struct Batch {
    PipelineKind             pipeline    = PIPE_SOLID;
    rhi::DescriptorSetHandle desc_set{};
    u32                      index_start = 0;
    u32                      index_count = 0;
};

struct HudRenderer::Impl {
    rhi::Rhi* rhi = nullptr;

    rhi::DescriptorSetLayoutHandle desc_layout{};
    rhi::SamplerHandle             sampler{};
    rhi::PipelineLayoutHandle      pipe_layout{};
    rhi::PipelineHandle            pipe_solid{};
    rhi::PipelineHandle            pipe_text{};

    rhi::TextureHandle       white_image{};
    rhi::DescriptorSetHandle white_set{};

    std::array<RingBuffer, rhi::MAX_FRAMES_IN_FLIGHT> rings{};

    std::vector<Vertex> verts;
    std::vector<u16>    inds;
    std::vector<Batch>  batches;

    bool frame_open = false;

    std::unique_ptr<Font> font;
    std::unordered_map<std::string, std::unique_ptr<HudImage>> images;
};

// ── Shader loading helper ─────────────────────────────────────────────────
static rhi::ShaderModuleHandle load_shader(rhi::Rhi& rhi, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return {};
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::error(TAG, "HUD shader not found: '{}'", path);
        return {};
    }
    return rhi.create_shader_module(bytes);
}

// ── Setup helpers ─────────────────────────────────────────────────────────

static bool create_descriptor_layout(HudRenderer::Impl& r) {
    rhi::DescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.type    = rhi::DescriptorType::CombinedImageSampler;
    b.count   = 1;
    b.stages  = rhi::ShaderStage::Fragment;
    rhi::DescriptorSetLayoutDesc desc{};
    desc.bindings = std::span{&b, 1};
    r.desc_layout = r.rhi->create_descriptor_set_layout(desc);
    return r.desc_layout.is_valid();
}

static bool create_sampler(HudRenderer::Impl& r) {
    rhi::SamplerDesc sd{};
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    sd.address_w = rhi::AddressMode::ClampToEdge;
    r.sampler = r.rhi->create_sampler(sd);
    return r.sampler.is_valid();
}

static bool create_pipeline_layout(HudRenderer::Impl& r) {
    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(glm::mat4);
    rhi::PipelineLayoutDesc d{};
    d.set_layouts    = std::span{&r.desc_layout, 1};
    d.push_constants = std::span{&pc, 1};
    r.pipe_layout = r.rhi->create_pipeline_layout(d);
    return r.pipe_layout.is_valid();
}

static bool create_pipeline_variant(HudRenderer::Impl& r, std::string_view frag_path, rhi::PipelineHandle& out) {
    auto vert_h = load_shader(*r.rhi, "engine/shaders/hud.vert.spv");
    auto frag_h = load_shader(*r.rhi, frag_path);
    if (!r.rhi->resolve(vert_h) || !r.rhi->resolve(frag_h)) {
        r.rhi->destroy_shader_module(vert_h);
        r.rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(Vertex), false };
    rhi::VertexAttributeDesc attrs[3]{
        { 0, 0, offsetof(Vertex, pos),   rhi::TextureFormat::R32G32_SFLOAT  },
        { 1, 0, offsetof(Vertex, color), rhi::TextureFormat::R8G8B8A8_UNORM },
        { 2, 0, offsetof(Vertex, uv),    rhi::TextureFormat::R32G32_SFLOAT  },
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
    ms.sample_count = static_cast<u32>(r.rhi->msaa_samples());

    rhi::TextureFormat color_fmt = r.rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = r.rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = r.pipe_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.topology          = rhi::PrimitiveTopology::TriangleList;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    out = r.rhi->create_graphics_pipeline(desc);
    r.rhi->destroy_shader_module(vert_h);
    r.rhi->destroy_shader_module(frag_h);
    return out.is_valid();
}

static bool create_ring_buffers(HudRenderer::Impl& r) {
    for (auto& ring : r.rings) {
        {
            rhi::BufferDesc d{};
            d.size   = MAX_VERTS * sizeof(Vertex);
            d.usage  = rhi::BufferUsage::Vertex;
            d.memory = rhi::MemoryUsage::HostSequential;
            ring.vb = r.rhi->create_buffer(d);
            if (!ring.vb.is_valid()) { log::error(TAG, "ring VB create failed"); return false; }
        }
        {
            rhi::BufferDesc d{};
            d.size   = MAX_INDS * sizeof(u16);
            d.usage  = rhi::BufferUsage::Index;
            d.memory = rhi::MemoryUsage::HostSequential;
            ring.ib = r.rhi->create_buffer(d);
            if (!ring.ib.is_valid()) { log::error(TAG, "ring IB create failed"); return false; }
        }
    }
    return true;
}

static bool create_white_texture(HudRenderer::Impl& r) {
    const u8 pixel[4] = {255, 255, 255, 255};
    constexpr u64 size = 4;

    rhi::BufferDesc bd{};
    bd.size   = size;
    bd.usage  = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::HostSequential;
    auto stage = r.rhi->create_buffer(bd);
    if (!stage.is_valid()) return false;
    std::memcpy(r.rhi->mapped_ptr(stage), pixel, size);

    {
        rhi::TextureDesc td{};
        td.width  = 1;
        td.height = 1;
        td.format = rhi::TextureFormat::R8G8B8A8_UNORM;
        td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        r.white_image = r.rhi->create_texture(td);
        if (!r.white_image.is_valid()) {
            r.rhi->destroy_buffer(stage);
            return false;
        }
    }
    rhi::CommandList cmd = r.rhi->begin_oneshot();
    {
        rhi::ImageBarrier b{};
        b.image      = r.white_image;
        b.src_stage  = rhi::PipelineStage::TopOfPipe;
        b.dst_stage  = rhi::PipelineStage::Transfer;
        b.dst_access = rhi::AccessFlag::TransferWrite;
        b.old_layout = rhi::ImageLayout::Undefined;
        b.new_layout = rhi::ImageLayout::TransferDstOptimal;
        cmd.image_barrier(b);

        rhi::BufferImageCopy copy{};
        cmd.copy_buffer_to_image(stage, r.white_image, std::span{&copy, 1});

        rhi::ImageBarrier b2{};
        b2.image      = r.white_image;
        b2.src_stage  = rhi::PipelineStage::Transfer;
        b2.src_access = rhi::AccessFlag::TransferWrite;
        b2.dst_stage  = rhi::PipelineStage::FragmentShader;
        b2.dst_access = rhi::AccessFlag::ShaderRead;
        b2.old_layout = rhi::ImageLayout::TransferDstOptimal;
        b2.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        cmd.image_barrier(b2);
    }
    r.rhi->end_oneshot(cmd);
    r.rhi->destroy_buffer(stage);

    r.white_set = r.rhi->allocate_descriptor_set(r.desc_layout);
    if (!r.white_set.is_valid()) return false;
    rhi::WriteDescriptor wd{};
    wd.binding = 0;
    wd.type    = rhi::DescriptorType::CombinedImageSampler;
    wd.texture = r.white_image;
    wd.sampler = r.sampler;
    r.rhi->update_descriptor_set(r.white_set, std::span{&wd, 1});
    return true;
}

// ── Image cache ───────────────────────────────────────────────────────────

static bool create_hud_image(HudRenderer::Impl& r, const u8* rgba, u32 w, u32 h, HudImage& out) {
    u64 size = static_cast<u64>(w) * h * 4;

    rhi::BufferDesc bd{};
    bd.size   = size;
    bd.usage  = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::HostSequential;
    auto stage = r.rhi->create_buffer(bd);
    if (!stage.is_valid()) return false;
    std::memcpy(r.rhi->mapped_ptr(stage), rgba, size);

    {
        rhi::TextureDesc td{};
        td.width  = w;
        td.height = h;
        td.format = rhi::TextureFormat::R8G8B8A8_SRGB;
        td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        out.handle = r.rhi->create_texture(td);
        if (!out.handle.is_valid()) {
            r.rhi->destroy_buffer(stage);
            return false;
        }
    }
    rhi::CommandList cmd = r.rhi->begin_oneshot();
    {
        rhi::ImageBarrier b{};
        b.image      = out.handle;
        b.src_stage  = rhi::PipelineStage::TopOfPipe;
        b.dst_stage  = rhi::PipelineStage::Transfer;
        b.dst_access = rhi::AccessFlag::TransferWrite;
        b.old_layout = rhi::ImageLayout::Undefined;
        b.new_layout = rhi::ImageLayout::TransferDstOptimal;
        cmd.image_barrier(b);

        rhi::BufferImageCopy copy{};
        copy.image_extent_w = w;
        copy.image_extent_h = h;
        cmd.copy_buffer_to_image(stage, out.handle, std::span{&copy, 1});

        rhi::ImageBarrier b2{};
        b2.image      = out.handle;
        b2.src_stage  = rhi::PipelineStage::Transfer;
        b2.src_access = rhi::AccessFlag::TransferWrite;
        b2.dst_stage  = rhi::PipelineStage::FragmentShader;
        b2.dst_access = rhi::AccessFlag::ShaderRead;
        b2.old_layout = rhi::ImageLayout::TransferDstOptimal;
        b2.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        cmd.image_barrier(b2);
    }
    r.rhi->end_oneshot(cmd);
    r.rhi->destroy_buffer(stage);

    out.set = r.rhi->allocate_descriptor_set(r.desc_layout);
    if (!out.set.is_valid()) {
        r.rhi->destroy_texture(out.handle);
        return false;
    }
    rhi::WriteDescriptor wd{};
    wd.binding = 0;
    wd.type    = rhi::DescriptorType::CombinedImageSampler;
    wd.texture = out.handle;
    wd.sampler = r.sampler;
    r.rhi->update_descriptor_set(out.set, std::span{&wd, 1});

    out.w = w;
    out.h = h;
    return true;
}

static void destroy_hud_images(HudRenderer::Impl& r) {
    if (!r.rhi) { r.images.clear(); return; }
    for (auto& [path, img] : r.images) {
        if (!img) continue;
        if (img->set.is_valid()) r.rhi->free_descriptor_set(img->set);
        r.rhi->destroy_texture(img->handle);
    }
    r.images.clear();
}

static HudImage* get_or_load_image(HudRenderer::Impl& r, std::string_view path) {
    std::string key{path};
    auto it = r.images.find(key);
    if (it != r.images.end()) return it->second.get();

    auto* mgr = asset::AssetManager::instance();
    if (!mgr) { r.images.emplace(std::move(key), nullptr); return nullptr; }
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::warn(TAG, "image not found: '{}'", path);
        r.images.emplace(std::move(key), nullptr);
        return nullptr;
    }
    auto decoded = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
    if (!decoded) {
        log::warn(TAG, "image decode failed '{}': {}", path, decoded.error());
        r.images.emplace(std::move(key), nullptr);
        return nullptr;
    }
    auto img = std::make_unique<HudImage>();
    if (!create_hud_image(r, decoded->pixels.data(), decoded->width, decoded->height, *img)) {
        log::warn(TAG, "image upload failed: '{}'", path);
        r.images.emplace(std::move(key), nullptr);
        return nullptr;
    }
    HudImage* raw = img.get();
    r.images.emplace(std::move(key), std::move(img));
    return raw;
}

// ── Batcher helpers ──────────────────────────────────────────────────────

static void ensure_batch(HudRenderer::Impl& r, PipelineKind pipe, rhi::DescriptorSetHandle set) {
    if (!r.batches.empty()) {
        Batch& last = r.batches.back();
        if (last.pipeline == pipe && last.desc_set == set) return;
        last.index_count = static_cast<u32>(r.inds.size()) - last.index_start;
    }
    Batch nb{};
    nb.pipeline    = pipe;
    nb.desc_set    = set;
    nb.index_start = static_cast<u32>(r.inds.size());
    nb.index_count = 0;
    r.batches.push_back(nb);
}

static u32 premul_rgba(Color c) {
    u8 r = (c.rgba >>  0) & 0xFF;
    u8 g = (c.rgba >>  8) & 0xFF;
    u8 b = (c.rgba >> 16) & 0xFF;
    u8 a = (c.rgba >> 24) & 0xFF;
    r = static_cast<u8>((u32(r) * a) / 255);
    g = static_cast<u8>((u32(g) * a) / 255);
    b = static_cast<u8>((u32(b) * a) / 255);
    return u32(r) | (u32(g) << 8) | (u32(b) << 16) | (u32(a) << 24);
}

static void append_triangle(HudRenderer::Impl& r,
                            f32 x0, f32 y0,
                            f32 x1, f32 y1,
                            f32 x2, f32 y2,
                            f32 u,  f32 v,
                            u32 premul) {
    if (r.verts.size() + 3 > MAX_VERTS) return;
    u16 base = static_cast<u16>(r.verts.size());
    r.verts.push_back({ { x0, y0 }, premul, { u, v } });
    r.verts.push_back({ { x1, y1 }, premul, { u, v } });
    r.verts.push_back({ { x2, y2 }, premul, { u, v } });
    r.inds.push_back(base + 0);
    r.inds.push_back(base + 1);
    r.inds.push_back(base + 2);
}

static void append_triangle_uv(HudRenderer::Impl& r,
                               f32 x0, f32 y0, f32 u0, f32 v0,
                               f32 x1, f32 y1, f32 u1, f32 v1,
                               f32 x2, f32 y2, f32 u2, f32 v2,
                               u32 premul) {
    if (r.verts.size() + 3 > MAX_VERTS) return;
    u16 base = static_cast<u16>(r.verts.size());
    r.verts.push_back({ { x0, y0 }, premul, { u0, v0 } });
    r.verts.push_back({ { x1, y1 }, premul, { u1, v1 } });
    r.verts.push_back({ { x2, y2 }, premul, { u2, v2 } });
    r.inds.push_back(base + 0);
    r.inds.push_back(base + 1);
    r.inds.push_back(base + 2);
}

static void append_quad(HudRenderer::Impl& r, const Rect& rc,
                        f32 u0, f32 v0, f32 u1, f32 v1, u32 premul) {
    if (r.verts.size() + 4 > MAX_VERTS) return;
    u16 base = static_cast<u16>(r.verts.size());
    r.verts.push_back({ { rc.x,        rc.y       }, premul, { u0, v0 } });
    r.verts.push_back({ { rc.x + rc.w, rc.y       }, premul, { u1, v0 } });
    r.verts.push_back({ { rc.x + rc.w, rc.y + rc.h }, premul, { u1, v1 } });
    r.verts.push_back({ { rc.x,        rc.y + rc.h }, premul, { u0, v1 } });
    r.inds.push_back(base + 0);
    r.inds.push_back(base + 1);
    r.inds.push_back(base + 2);
    r.inds.push_back(base + 0);
    r.inds.push_back(base + 2);
    r.inds.push_back(base + 3);
}

// UTF-8 decoder shared by draw_text + text_width_px.
static u32 utf8_next(const char*& p, const char* end) {
    if (p >= end) return 0;
    u8 b0 = static_cast<u8>(*p++);
    if (b0 < 0x80) return b0;
    int extra = 0;
    u32 cp = 0;
    if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1Fu; extra = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0Fu; extra = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07u; extra = 3; }
    else return 0xFFFDu;
    for (int i = 0; i < extra; ++i) {
        if (p >= end) return 0xFFFDu;
        u8 bx = static_cast<u8>(*p++);
        if ((bx & 0xC0) != 0x80) return 0xFFFDu;
        cp = (cp << 6) | (bx & 0x3Fu);
    }
    return cp;
}

// ── Circle / disc / ring primitives ───────────────────────────────────────

static void draw_filled_circle(HudRenderer::Impl& r, f32 cx, f32 cy, f32 radius, Color color) {
    if (radius <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(r, PIPE_SOLID, r.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);

    f32 px0 = cx + radius;
    f32 py0 = cy;
    for (u32 i = 0; i < kSegments; ++i) {
        f32 a = step * static_cast<f32>(i + 1);
        f32 px1 = cx + std::cos(a) * radius;
        f32 py1 = cy + std::sin(a) * radius;
        append_triangle(r, cx, cy, px0, py0, px1, py1, 0.0f, 0.0f, premul);
        px0 = px1;
        py0 = py1;
    }
}

static void draw_disc(HudRenderer::Impl& r, f32 cx, f32 cy, f32 radius, Color color) {
    if (radius <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(r, PIPE_SOLID, r.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);
    for (u32 i = 0; i < kSegments; ++i) {
        f32 a0 = step * static_cast<f32>(i);
        f32 a1 = step * static_cast<f32>(i + 1);
        f32 x0 = cx + std::cos(a0) * radius;
        f32 y0 = cy + std::sin(a0) * radius;
        f32 x1 = cx + std::cos(a1) * radius;
        f32 y1 = cy + std::sin(a1) * radius;
        append_triangle(r, cx, cy, x0, y0, x1, y1, 0.0f, 0.0f, premul);
    }
}

static void draw_ring_arc(HudRenderer::Impl& r, f32 cx, f32 cy, f32 r_outer, f32 r_inner,
                          f32 start_angle, f32 sweep_angle, Color color) {
    if (r_outer <= r_inner || r_outer <= 0.0f) return;
    if (sweep_angle <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(r, PIPE_SOLID, r.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegmentsFull = 48;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 frac = sweep_angle / TWO_PI;
    if (frac > 1.0f) frac = 1.0f;
    u32 n = static_cast<u32>(std::ceil(static_cast<f32>(kSegmentsFull) * frac));
    if (n < 1) n = 1;
    f32 step = sweep_angle / static_cast<f32>(n);
    for (u32 i = 0; i < n; ++i) {
        f32 a0 = start_angle + step * static_cast<f32>(i);
        f32 a1 = start_angle + step * static_cast<f32>(i + 1);
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        f32 ox0 = cx + c0 * r_outer, oy0 = cy + s0 * r_outer;
        f32 ox1 = cx + c1 * r_outer, oy1 = cy + s1 * r_outer;
        f32 ix0 = cx + c0 * r_inner, iy0 = cy + s0 * r_inner;
        f32 ix1 = cx + c1 * r_inner, iy1 = cy + s1 * r_inner;
        append_triangle(r, ox0, oy0, ox1, oy1, ix1, iy1, 0.0f, 0.0f, premul);
        append_triangle(r, ox0, oy0, ix1, iy1, ix0, iy0, 0.0f, 0.0f, premul);
    }
}

static void draw_ring(HudRenderer::Impl& r, f32 cx, f32 cy, f32 r_outer, f32 r_inner, Color color) {
    if (r_outer <= r_inner || r_outer <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(r, PIPE_SOLID, r.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);

    for (u32 i = 0; i < kSegments; ++i) {
        f32 a0 = step * static_cast<f32>(i);
        f32 a1 = step * static_cast<f32>(i + 1);
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        f32 ox0 = cx + c0 * r_outer, oy0 = cy + s0 * r_outer;
        f32 ox1 = cx + c1 * r_outer, oy1 = cy + s1 * r_outer;
        f32 ix0 = cx + c0 * r_inner, iy0 = cy + s0 * r_inner;
        f32 ix1 = cx + c1 * r_inner, iy1 = cy + s1 * r_inner;
        append_triangle(r, ox0, oy0, ox1, oy1, ix1, iy1, 0.0f, 0.0f, premul);
        append_triangle(r, ox0, oy0, ix1, iy1, ix0, iy0, 0.0f, 0.0f, premul);
    }
}

static Color scale_color_alpha(Color c, f32 frac) {
    if (frac >= 1.0f) return c;
    if (frac <= 0.0f) frac = 0.0f;
    u32 a = (c.rgba >> 24) & 0xFFu;
    u32 a2 = static_cast<u32>(static_cast<f32>(a) * frac + 0.5f);
    if (a2 > 255u) a2 = 255u;
    return Color{ (c.rgba & 0x00FFFFFFu) | (a2 << 24) };
}

// ── Slot resolution + affordability ──────────────────────────────────────
// Pure-data helpers used by both render walks and (in hud.cpp) input
// handling. Duplicated here so each TU can call them without exporting
// a shared symbol — they're tiny and read-only.

static const simulation::AbilityInstance*
resolve_slot_ability(u32 slot_index,
                     const ActionBarConfig& cfg,
                     const WorldContext& ctx,
                     const simulation::AbilityDef*& out_def) {
    out_def = nullptr;
    if (slot_index >= cfg.slots.size()) return nullptr;
    if (!ctx.selection || !ctx.world || !ctx.abilities) return nullptr;
    const auto& sel = ctx.selection->selected();
    if (sel.empty()) return nullptr;
    const auto* aset = ctx.world->ability_sets.get(sel.front().id);
    if (!aset) return nullptr;

    const auto& slot = cfg.slots[slot_index];

    if (cfg.binding_mode == ActionBarBindingMode::Manual) {
        if (slot.bound_ability.empty()) return nullptr;
        for (const auto& inst : aset->abilities) {
            if (inst.ability_id == slot.bound_ability) {
                const auto* def = ctx.abilities->get(inst.ability_id);
                if (!def) return nullptr;
                out_def = def;
                return &inst;
            }
        }
        return nullptr;
    }

    u32 nth = 0;
    for (const auto& inst : aset->abilities) {
        const auto* def = ctx.abilities->get(inst.ability_id);
        if (!def || def->hidden || inst.from_item) continue;
        if (nth == slot_index) {
            out_def = def;
            return &inst;
        }
        ++nth;
    }
    return nullptr;
}

static bool can_afford(const simulation::World& world, u32 unit_id,
                       const simulation::AbilityLevelDef& lvl) {
    if (lvl.cost.empty()) return true;
    for (const auto& [state_name, amount] : lvl.cost) {
        if (amount <= 0.0f) continue;
        if (state_name == "health") {
            const auto* hp = world.healths.get(unit_id);
            if (!hp || hp->current < amount) return false;
            continue;
        }
        const auto* sb = world.state_blocks.get(unit_id);
        if (!sb) return false;
        auto it = sb->states.find(state_name);
        if (it == sb->states.end() || it->second.current < amount) return false;
    }
    return true;
}

// slot_castable_now intentionally not duplicated here — the renderer
// gates icon dim / pulse via can_afford + cooldown_remaining inline
// inside each composite's draw helper.

static const simulation::Inventory*
inventory_resolve_selected(const Hud::Impl& s, u32* out_carrier_id = nullptr) {
    if (out_carrier_id) *out_carrier_id = UINT32_MAX;
    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->selection) return nullptr;
    const auto& sel = s.world_ctx->selection->selected();
    if (sel.empty()) return nullptr;
    u32 id = sel.front().id;
    const auto* inv = s.world_ctx->world->inventories.get(id);
    if (inv && out_carrier_id) *out_carrier_id = id;
    return inv;
}

static bool inventory_resolve_slot(const Hud::Impl& s,
                                   const simulation::Inventory* inv,
                                   u32 slot_index,
                                   simulation::Item& out_item,
                                   const simulation::ItemInfo*& out_info,
                                   const simulation::ItemTypeDef*& out_def) {
    out_item = {};
    out_info = nullptr;
    out_def  = nullptr;
    if (!inv || slot_index >= inv->slots.size()) return false;
    simulation::Item item = inv->slots[slot_index];
    if (!item.is_valid() || !s.world_ctx || !s.world_ctx->world) return false;
    const auto* info = s.world_ctx->world->item_infos.get(item.id);
    if (!info) return false;
    out_item = item;
    out_info = info;
    if (s.world_ctx->types) out_def = s.world_ctx->types->get_item_type(info->type_id);
    return true;
}

static bool command_bar_slots_active(const Hud::Impl& s) {
    if (!s.world_ctx || !s.world_ctx->selection ||
        s.world_ctx->selection->selected().empty()) return false;
    u32 lead = s.world_ctx->selection->selected().front().id;
    const auto* own = s.world_ctx->world ? s.world_ctx->world->owners.get(lead) : nullptr;
    return own && own->player.id == s.world_ctx->local_player.id;
}

// ── Inline draw primitive impls used by composite helpers below ──────────
// These mirror HudRenderer::draw_rect / draw_image / draw_text but take
// the renderer Impl directly so composite helpers can call them without
// re-entering through a HudRenderer reference at every site.

static void emit_rect(HudRenderer::Impl& r, const Rect& rc, Color color) {
    if (!r.frame_open) return;
    ensure_batch(r, PIPE_SOLID, r.white_set);
    append_quad(r, rc, 0.0f, 0.0f, 1.0f, 1.0f, premul_rgba(color));
}

static void emit_image(HudRenderer::Impl& r, const Rect& rc,
                       std::string_view asset_path, Color tint) {
    if (!r.frame_open) return;
    HudImage* img = get_or_load_image(r, asset_path);
    if (!img) return;
    ensure_batch(r, PIPE_SOLID, img->set);
    append_quad(r, rc, 0.0f, 0.0f, 1.0f, 1.0f, premul_rgba(tint));
}

static void emit_image_disc(HudRenderer::Impl& r, f32 cx, f32 cy, f32 radius,
                            std::string_view asset_path, Color tint) {
    if (!r.frame_open) return;
    if (radius <= 0.0f) return;
    HudImage* img = get_or_load_image(r, asset_path);
    if (!img) return;
    ensure_batch(r, PIPE_SOLID, img->set);
    u32 premul = premul_rgba(tint);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);
    for (u32 i = 0; i < kSegments; ++i) {
        f32 a0 = step * static_cast<f32>(i);
        f32 a1 = step * static_cast<f32>(i + 1);
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        append_triangle_uv(
            r,
            cx,                   cy,                   0.5f,            0.5f,
            cx + c0 * radius,     cy + s0 * radius,     0.5f + c0 * 0.5f, 0.5f + s0 * 0.5f,
            cx + c1 * radius,     cy + s1 * radius,     0.5f + c1 * 0.5f, 0.5f + s1 * 0.5f,
            premul);
    }
}

static void emit_text(HudRenderer::Impl& r, f32 x_left, f32 y_baseline,
                      std::string_view utf8, Color color, f32 px_size) {
    if (!r.frame_open) return;
    if (!r.font || !r.font->valid()) return;

    ensure_batch(r, PIPE_TEXT, r.font->atlas_descriptor());
    u32 premul = premul_rgba(color);
    f32 pen = x_left;

    const char* p   = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        u32 cp = utf8_next(p, end);
        if (cp == 0) break;
        if (cp == '\n') continue;
        if (cp == '\t') cp = ' ';

        const Font::Glyph* g = r.font->get_glyph(cp);
        if (!g) continue;
        if (g->plane_w > 0.0f && g->plane_h > 0.0f) {
            f32 x0 = pen + g->bearing_x * px_size;
            f32 y0 = y_baseline - g->bearing_y * px_size;
            Rect qr{ x0, y0, g->plane_w * px_size, g->plane_h * px_size };
            append_quad(r, qr, g->uv0[0], g->uv0[1], g->uv1[0], g->uv1[1], premul);
        }
        pen += g->advance * px_size;
    }
}

static f32 measure_text(const HudRenderer::Impl& r,
                        std::string_view utf8, f32 px_size) {
    if (!r.font || !r.font->valid()) return 0.0f;
    auto& font = *r.font;
    f32 width = 0.0f;
    const char* p   = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        u32 cp = utf8_next(p, end);
        if (cp == 0 || cp == '\n') break;
        if (cp == '\t') cp = ' ';
        const Font::Glyph* g = font.get_glyph(cp);
        if (!g) continue;
        width += g->advance * px_size;
    }
    return width;
}

static f32 measure_ascent(const HudRenderer::Impl& r, f32 px_size) {
    if (!r.font || !r.font->valid()) return 0.0f;
    return r.font->ascent() * px_size;
}

static f32 measure_line_height(const HudRenderer::Impl& r, f32 px_size) {
    if (!r.font || !r.font->valid()) return 0.0f;
    return r.font->line_height() * px_size;
}

// ── Cooldown pie (perimeter-clipped sector) ──────────────────────────────

static void perimeter_point(f32 cx, f32 cy, f32 hw, f32 hh,
                             f32 theta, f32& out_x, f32& out_y) {
    f32 dx = std::sin(theta);
    f32 dy = -std::cos(theta);
    f32 tx = (std::abs(dx) > 1e-5f) ? hw / std::abs(dx) : 1e9f;
    f32 ty = (std::abs(dy) > 1e-5f) ? hh / std::abs(dy) : 1e9f;
    f32 t  = std::min(tx, ty);
    out_x = cx + dx * t;
    out_y = cy + dy * t;
}

static void draw_cooldown_pie(HudRenderer::Impl& r, const Rect& rc, f32 fraction, Color overlay) {
    if (fraction <= 0.0f) return;
    if (fraction > 1.0f)  fraction = 1.0f;
    ensure_batch(r, PIPE_SOLID, r.white_set);
    u32 premul = premul_rgba(overlay);

    f32 cx = rc.x + rc.w * 0.5f;
    f32 cy = rc.y + rc.h * 0.5f;
    f32 hw = rc.w * 0.5f;
    f32 hh = rc.h * 0.5f;

    constexpr u32 kSegmentsFull = 48;
    u32 n = static_cast<u32>(std::ceil(kSegmentsFull * fraction));
    if (n < 1) n = 1;

    constexpr f32 TWO_PI = 6.2831853f;
    f32 start = (1.0f - fraction) * TWO_PI;
    f32 step  = (TWO_PI - start) / static_cast<f32>(n);

    f32 px0, py0;
    perimeter_point(cx, cy, hw, hh, start, px0, py0);
    for (u32 i = 0; i < n; ++i) {
        f32 a = start + step * static_cast<f32>(i + 1);
        f32 px1, py1;
        perimeter_point(cx, cy, hw, hh, a, px1, py1);
        append_triangle(r, cx, cy, px0, py0, px1, py1, 0.0f, 0.0f, premul);
        px0 = px1;
        py0 = py1;
    }
}

static void format_cooldown_secs(f32 remaining, char* buf, size_t buf_size) {
    if (remaining >= 1.0f) {
        int secs = static_cast<int>(std::ceil(remaining));
        std::snprintf(buf, buf_size, "%d", secs);
    } else if (remaining > 0.0f) {
        std::snprintf(buf, buf_size, "%.1f", remaining);
    } else {
        buf[0] = '\0';
    }
}

// ── Action-bar render variants ───────────────────────────────────────────

static void draw_action_bar_classic_rts(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    const auto& rt  = s.action_bar_rt;

    bool any_armed = !s.action_bar_targeting_ability.empty();

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;

        const simulation::AbilityDef* def = nullptr;
        const simulation::AbilityInstance* inst = nullptr;
        if (s.world_ctx) inst = resolve_slot_ability(i, cfg, *s.world_ctx, def);

        bool armed = any_armed && def && inst
                  && inst->ability_id == s.action_bar_targeting_ability;

        bool has_button = (inst && def);
        Color bg = slot.style.bg;
        if (has_button && slot.pressed)      bg = slot.style.press_bg;
        else if (armed)                      bg = slot.style.press_bg;
        else if (has_button && slot.hovered) bg = slot.style.hover_bg;
        emit_rect(r, slot.rect, bg);

        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        if (def && !def->icon.empty()) {
            emit_image(r, icon_rect, def->icon, rgba(255, 255, 255, 255));
        }

        if (armed) {
            // Armed slot: no cooldown / affordability overlay.
        } else if (any_armed) {
            emit_rect(r, icon_rect, cfg.style.disabled_tint);
        } else {
            bool on_cooldown = def && inst && inst->cooldown_remaining > 0.05f
                            && def->level_data(inst->level).cooldown > 0.0f;
            if (on_cooldown) {
                f32 total = def->level_data(inst->level).cooldown;
                f32 frac  = inst->cooldown_remaining / total;
                draw_cooldown_pie(r, icon_rect, frac, cfg.style.cooldown_overlay);

                char buf[16];
                format_cooldown_secs(inst->cooldown_remaining, buf, sizeof(buf));
                if (buf[0] != '\0') {
                    f32 px = cfg.style.cooldown_text_size;
                    f32 tw = measure_text(r, buf, px);
                    f32 line_h = measure_line_height(r, px);
                    f32 ascent = measure_ascent(r, px);
                    f32 tx = icon_rect.x + (icon_rect.w - tw) * 0.5f;
                    f32 ty = icon_rect.y + (icon_rect.h - line_h) * 0.5f + ascent;
                    emit_text(r, tx, ty, buf, cfg.style.cooldown_text_color, px);
                }
            } else if (def && inst && s.world_ctx && is_castable_form(def->form)) {
                u32 unit_id = s.world_ctx->selection
                                ? s.world_ctx->selection->selected().front().id
                                : UINT32_MAX;
                if (unit_id != UINT32_MAX && s.world_ctx->world) {
                    if (!can_afford(*s.world_ctx->world, unit_id,
                                    def->level_data(inst->level))) {
                        emit_rect(r, icon_rect, cfg.style.disabled_tint);
                    }
                }
            }
        }

        Color border_color = slot.style.border_color;
        f32   border_w     = bw;
        if (armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            Rect rc = slot.rect;
            emit_rect(r, { rc.x, rc.y, rc.w, border_w }, border_color);
            emit_rect(r, { rc.x, rc.y + rc.h - border_w, rc.w, border_w }, border_color);
            emit_rect(r, { rc.x, rc.y, border_w, rc.h }, border_color);
            emit_rect(r, { rc.x + rc.w - border_w, rc.y, border_w, rc.h }, border_color);
        }

        bool show_hotkey = def && is_castable_form(def->form);
        std::string_view badge_key;
        if (show_hotkey) {
            badge_key = (rt.hotkey_mode == ActionBarHotkeyMode::Ability)
                            ? std::string_view{def->hotkey}
                            : std::string_view{slot.hotkey};
        }
        if (!badge_key.empty()) {
            f32 px_size = slot.rect.h * 0.28f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = measure_text(r, badge_key, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 ascent = measure_ascent(r, px_size);
            f32 y_base = slot.rect.y + ascent + 2.0f;

            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f;
                f32 pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x,
                    y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f,
                    ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.hotkey_badge_bg);
            }
            emit_text(r, x_left, y_base, badge_key, cfg.style.hotkey_color, px_size);
        }
    }
}

static void draw_action_bar_moba(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    const auto& rt  = s.action_bar_rt;

    bool any_armed = !s.action_bar_targeting_ability.empty();

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;

        const simulation::AbilityDef* def = nullptr;
        const simulation::AbilityInstance* inst = nullptr;
        if (s.world_ctx) inst = resolve_slot_ability(i, cfg, *s.world_ctx, def);

        bool armed = any_armed && def && inst
                  && inst->ability_id == s.action_bar_targeting_ability;
        bool has_button = (inst && def);

        f32 cx = slot.rect.x + slot.rect.w * 0.5f;
        f32 cy = slot.rect.y + slot.rect.h * 0.5f;
        f32 ring_w = cfg.style.cooldown_ring_width;
        f32 ring_g = cfg.style.cooldown_ring_gap;
        f32 button_r = std::min(slot.rect.w, slot.rect.h) * 0.5f - ring_w - ring_g;
        if (button_r <= 0.0f) button_r = std::min(slot.rect.w, slot.rect.h) * 0.4f;

        Color bg = slot.style.bg;
        if (has_button && slot.pressed)      bg = slot.style.press_bg;
        else if (armed)                      bg = slot.style.press_bg;
        else if (has_button && slot.hovered) bg = slot.style.hover_bg;
        draw_disc(r, cx, cy, button_r, bg);

        if (def && !def->icon.empty()) {
            emit_image_disc(r, cx, cy, button_r, def->icon, rgba(255, 255, 255, 255));
        }

        if (armed) {
        } else if (any_armed) {
            draw_disc(r, cx, cy, button_r, cfg.style.disabled_tint);
        } else {
            bool on_cooldown = def && inst && inst->cooldown_remaining > 0.05f
                            && def->level_data(inst->level).cooldown > 0.0f;
            if (on_cooldown) {
                f32 total = def->level_data(inst->level).cooldown;
                f32 frac  = inst->cooldown_remaining / total;

                draw_disc(r, cx, cy, button_r, cfg.style.cooldown_overlay);

                constexpr f32 TWO_PI = 6.2831853f;
                constexpr f32 TWELVE_OCLOCK = -1.5707963f;
                f32 r_outer = button_r + ring_g + ring_w;
                f32 r_inner = button_r + ring_g;
                f32 sweep   = frac * TWO_PI;
                draw_ring_arc(r, cx, cy, r_outer, r_inner,
                              TWELVE_OCLOCK, sweep, cfg.style.cooldown_ring_color);

                char buf[16];
                format_cooldown_secs(inst->cooldown_remaining, buf, sizeof(buf));
                if (buf[0] != '\0') {
                    f32 px = cfg.style.cooldown_text_size;
                    f32 tw = measure_text(r, buf, px);
                    f32 line_h = measure_line_height(r, px);
                    f32 ascent = measure_ascent(r, px);
                    f32 tx = cx - tw * 0.5f;
                    f32 ty = cy - line_h * 0.5f + ascent;
                    emit_text(r, tx, ty, buf, cfg.style.cooldown_text_color, px);
                }
            } else if (def && inst && s.world_ctx && is_castable_form(def->form)) {
                u32 unit_id = s.world_ctx->selection
                                ? s.world_ctx->selection->selected().front().id
                                : UINT32_MAX;
                if (unit_id != UINT32_MAX && s.world_ctx->world) {
                    if (!can_afford(*s.world_ctx->world, unit_id,
                                    def->level_data(inst->level))) {
                        draw_disc(r, cx, cy, button_r, cfg.style.disabled_tint);
                    }
                }
            }
        }

        Color border_color = slot.style.border_color;
        f32   border_w     = slot.style.border_width;
        if (armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            draw_ring(r, cx, cy, button_r, button_r - border_w, border_color);
        }

        bool show_hotkey = def && is_castable_form(def->form);
        std::string_view badge_key;
        if (show_hotkey) {
            badge_key = (rt.hotkey_mode == ActionBarHotkeyMode::Ability)
                            ? std::string_view{def->hotkey}
                            : std::string_view{slot.hotkey};
        }
        if (!badge_key.empty()) {
            f32 px_size = button_r * 0.55f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = measure_text(r, badge_key, px_size);
            f32 ascent = measure_ascent(r, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 y_base = slot.rect.y + slot.rect.h - 4.0f;

            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f;
                f32 pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x,
                    y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f,
                    ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.hotkey_badge_bg);
            }
            emit_text(r, x_left, y_base, badge_key, cfg.style.hotkey_color, px_size);
        }
    }
}

static void draw_action_bar_cancel_zone(HudRenderer::Impl& r, Hud::Impl& s) {
    using Phase = Hud::Impl::DragCastPhase;
    if (s.drag_cast.phase != Phase::Aiming &&
        s.drag_cast.phase != Phase::Cancelling) return;
    const auto& cfg = s.action_bar_cfg;
    const Rect& rc = cfg.cancel_zone_rect;
    if (rc.w <= 0.0f || rc.h <= 0.0f) return;

    bool active = (s.drag_cast.phase == Phase::Cancelling);
    Color bg     = active ? cfg.style.cancel_zone_active_bg     : cfg.style.cancel_zone_idle_bg;
    Color border = active ? cfg.style.cancel_zone_active_border : cfg.style.cancel_zone_idle_border;
    f32 cx = rc.x + rc.w * 0.5f;
    f32 cy = rc.y + rc.h * 0.5f;
    f32 radius = std::min(rc.w, rc.h) * 0.5f;
    f32 border_width = active ? 4.0f : 3.0f;

    draw_filled_circle(r, cx, cy, radius, bg);
    if ((border.rgba >> 24) != 0) {
        draw_ring(r, cx, cy, radius, radius - border_width, border);
    }

    f32 px_size = radius * 1.0f;
    if (px_size < 16.0f) px_size = 16.0f;
    std::string_view glyph = "X";
    f32 text_w = measure_text(r, glyph, px_size);
    f32 ascent = measure_ascent(r, px_size);
    f32 line_h = measure_line_height(r, px_size);
    f32 x_left = cx - text_w * 0.5f;
    f32 y_base = cy + ascent - line_h * 0.5f;
    emit_text(r, x_left, y_base, glyph, cfg.style.cancel_zone_glyph_color, px_size);
}

static void draw_action_bar(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    if (!cfg.enabled) return;
    if (!s.action_bar_rt.visible) return;

    if ((cfg.style.bg.rgba >> 24) != 0) {
        emit_rect(r, cfg.rect, cfg.style.bg);
    }

    switch (cfg.style_id) {
        case ActionBarStyleId::ClassicRts:
            draw_action_bar_classic_rts(r, s);
            break;
        case ActionBarStyleId::Moba:
            draw_action_bar_moba(r, s);
            break;
    }

    draw_action_bar_cancel_zone(r, s);
}

// ── Command bar ──────────────────────────────────────────────────────────

static void draw_command_bar_round(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.command_bar_cfg;

    bool slots_active = command_bar_slots_active(s);
    const std::string& armed = s.command_bar_armed_command;
    auto now = std::chrono::steady_clock::now();

    for (const auto& slot : cfg.slots) {
        if (!slot.visible) continue;
        bool is_armed = slots_active && (!armed.empty() && slot.command == armed);
        bool press_visual = slot.pressed || now < slot.press_pulse_until;

        f32 cx = slot.rect.x + slot.rect.w * 0.5f;
        f32 cy = slot.rect.y + slot.rect.h * 0.5f;
        f32 button_r = std::min(slot.rect.w, slot.rect.h) * 0.5f - slot.style.border_width;
        if (button_r <= 0.0f) button_r = std::min(slot.rect.w, slot.rect.h) * 0.5f;

        Color bg = slot.style.bg;
        if (slots_active && press_visual)      bg = slot.style.press_bg;
        else if (is_armed)                     bg = slot.style.press_bg;
        else if (slots_active && slot.hovered) bg = slot.style.hover_bg;
        draw_disc(r, cx, cy, button_r, bg);

        if (slots_active && !slot.icon.empty()) {
            emit_image_disc(r, cx, cy, button_r, slot.icon, rgba(255, 255, 255, 255));
        }

        Color border_color = slot.style.border_color;
        f32   border_w     = slot.style.border_width;
        if (is_armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            f32 r_outer = button_r + border_w;
            draw_ring_arc(r, cx, cy, r_outer, button_r, 0.0f, 6.2831853f, border_color);
        }

        if (slots_active && !slot.hotkey.empty()) {
            f32 px_size = button_r * 0.45f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = measure_text(r, slot.hotkey, px_size);
            f32 ascent = measure_ascent(r, px_size);
            f32 x_left = cx + button_r * 0.55f - text_w * 0.5f;
            f32 y_base = cy - button_r * 0.55f + ascent * 0.5f;
            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.hotkey_badge_bg);
            }
            emit_text(r, x_left, y_base, slot.hotkey, cfg.style.hotkey_color, px_size);
        }
    }
}

static void draw_command_bar(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.command_bar_cfg;
    if (!cfg.enabled || !s.command_bar_rt.visible) return;

    if ((cfg.style.bg.rgba >> 24) != 0) emit_rect(r, cfg.rect, cfg.style.bg);

    if (cfg.style_id == CommandBarStyleId::Round) {
        draw_command_bar_round(r, s);
        return;
    }

    bool slots_active = command_bar_slots_active(s);
    const std::string& armed = s.command_bar_armed_command;

    for (const auto& slot : cfg.slots) {
        if (!slot.visible) continue;
        bool is_armed = slots_active && (!armed.empty() && slot.command == armed);

        Color bg = slot.style.bg;
        if (slots_active && slot.pressed)      bg = slot.style.press_bg;
        else if (is_armed)                     bg = slot.style.press_bg;
        else if (slots_active && slot.hovered) bg = slot.style.hover_bg;
        emit_rect(r, slot.rect, bg);

        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        if (slots_active && !slot.icon.empty()) {
            emit_image(r, icon_rect, slot.icon, rgba(255, 255, 255, 255));
        }

        Color border_color = slot.style.border_color;
        f32   border_w     = bw;
        if (is_armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            Rect rc = slot.rect;
            emit_rect(r, { rc.x, rc.y, rc.w, border_w }, border_color);
            emit_rect(r, { rc.x, rc.y + rc.h - border_w, rc.w, border_w }, border_color);
            emit_rect(r, { rc.x, rc.y, border_w, rc.h }, border_color);
            emit_rect(r, { rc.x + rc.w - border_w, rc.y, border_w, rc.h }, border_color);
        }

        if (slots_active && !slot.hotkey.empty()) {
            f32 px_size = slot.rect.h * 0.28f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = measure_text(r, slot.hotkey, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 ascent = measure_ascent(r, px_size);
            f32 y_base = slot.rect.y + ascent + 2.0f;
            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.hotkey_badge_bg);
            }
            emit_text(r, x_left, y_base, slot.hotkey, cfg.style.hotkey_color, px_size);
        }
    }
}

// ── Joystick ─────────────────────────────────────────────────────────────

static void draw_joystick(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.joystick_cfg;
    const auto& rt  = s.joystick_rt;
    if (!cfg.enabled || !rt.visible) return;

    f32 cx = rt.base_cx;
    f32 cy = rt.base_cy;
    f32 base_r = std::min(cfg.rect.w, cfg.rect.h) * 0.5f;
    f32 knob_r = base_r * cfg.style.knob_size_frac * 0.5f;

    bool active = rt.captured_slot >= 0;
    f32 alpha_frac = active ? 1.0f : cfg.style.idle_alpha_frac;

    draw_filled_circle(r, cx, cy, base_r,
                       scale_color_alpha(cfg.style.base_color, alpha_frac));
    if (cfg.style.base_border_width > 0.0f) {
        draw_ring(r, cx, cy, base_r, base_r - cfg.style.base_border_width,
                  scale_color_alpha(cfg.style.base_border, alpha_frac));
    }

    f32 kx = cx + rt.knob_dx;
    f32 ky = cy + rt.knob_dy;
    draw_filled_circle(r, kx, ky, knob_r,
                       scale_color_alpha(cfg.style.knob_color, alpha_frac));
    if (cfg.style.knob_border_width > 0.0f) {
        draw_ring(r, kx, ky, knob_r, knob_r - cfg.style.knob_border_width,
                  scale_color_alpha(cfg.style.knob_border, alpha_frac));
    }
}

// ── Minimap ──────────────────────────────────────────────────────────────

static Color minimap_dot_color(const WorldContext& ctx, u32 unit_id,
                               const MinimapStyle& style) {
    if (!ctx.world) return style.neutral_dot_color;
    const auto* owner = ctx.world->owners.get(unit_id);
    if (!owner) return style.neutral_dot_color;
    simulation::Player p = owner->player;
    if (p == ctx.local_player) return style.own_dot_color;
    return style.enemy_dot_color;
}

static void draw_minimap(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.minimap_cfg;
    if (!cfg.enabled || !s.minimap_rt.visible) return;

    emit_rect(r, cfg.rect, cfg.style.bg);
    f32 bw = cfg.style.border_width;
    if (bw > 0.0f && (cfg.style.border_color.rgba >> 24) != 0) {
        const Rect& rc = cfg.rect;
        emit_rect(r, { rc.x, rc.y, rc.w, bw }, cfg.style.border_color);
        emit_rect(r, { rc.x, rc.y + rc.h - bw, rc.w, bw }, cfg.style.border_color);
        emit_rect(r, { rc.x, rc.y, bw, rc.h }, cfg.style.border_color);
        emit_rect(r, { rc.x + rc.w - bw, rc.y, bw, rc.h }, cfg.style.border_color);
    }

    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->terrain) return;
    const auto& world = *s.world_ctx->world;
    const auto& td    = *s.world_ctx->terrain;
    const auto* vision = s.world_ctx->vision;

    f32 inv_w = (td.world_width()  > 0.0f) ? (cfg.rect.w / td.world_width())  : 0.0f;
    f32 inv_h = (td.world_height() > 0.0f) ? (cfg.rect.h / td.world_height()) : 0.0f;

    for (u32 i = 0; i < world.transforms.count(); ++i) {
        u32 id = world.transforms.ids()[i];
        const auto& tf = world.transforms.data()[i];

        const auto* info = world.handle_infos.get(id);
        if (!info || info->category != simulation::Category::Unit) continue;
        if (const auto* hp = world.healths.get(id); hp && hp->current <= 0.0f) continue;

        if (vision) {
            i32 tx = static_cast<i32>((tf.position.x - td.origin_x()) / td.tile_size);
            i32 ty = static_cast<i32>((tf.position.y - td.origin_y()) / td.tile_size);
            if (tx >= 0 && ty >= 0 &&
                static_cast<u32>(tx) < td.tiles_x &&
                static_cast<u32>(ty) < td.tiles_y) {
                if (!vision->is_visible(s.world_ctx->local_player,
                                        static_cast<u32>(tx), static_cast<u32>(ty))) {
                    continue;
                }
            }
        }

        f32 sx = cfg.rect.x + (tf.position.x - td.origin_x()) * inv_w;
        f32 sy = cfg.rect.y + cfg.rect.h - (tf.position.y - td.origin_y()) * inv_h;
        f32 half = cfg.style.dot_size * 0.5f;
        Rect dot{ sx - half, sy - half, cfg.style.dot_size, cfg.style.dot_size };
        emit_rect(r, dot, minimap_dot_color(*s.world_ctx, id, cfg.style));
    }
}

// ── Inventory ────────────────────────────────────────────────────────────

static void draw_inventory(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.inventory_cfg;
    if (!cfg.enabled || !s.inventory_rt.visible) return;

    if ((cfg.style.bg.rgba >> 24) != 0) emit_rect(r, cfg.rect, cfg.style.bg);

    u32 carrier_id = UINT32_MAX;
    const simulation::Inventory* inv = inventory_resolve_selected(s, &carrier_id);

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;

        simulation::Item item;
        const simulation::ItemInfo*    info = nullptr;
        const simulation::ItemTypeDef* def  = nullptr;
        bool has_item = inventory_resolve_slot(s, inv, i, item, info, def);

        bool available = inv && i < inv->slots.size();

        Color bg;
        if (!available)        bg = slot.style.unavailable_bg;
        else if (has_item)     bg = slot.style.bg;
        else                   bg = slot.style.empty_bg;
        if (has_item && slot.pressed)      bg = slot.style.press_bg;
        else if (has_item && slot.hovered) bg = slot.style.hover_bg;
        emit_rect(r, slot.rect, bg);

        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        if (def && !def->icon_path.empty()) {
            emit_image(r, icon_rect, def->icon_path, rgba(255, 255, 255, 255));
        }

        if (def && !def->abilities.empty() && carrier_id != UINT32_MAX
            && s.world_ctx && s.world_ctx->world && s.world_ctx->abilities) {
            const std::string& fa = def->abilities[0];
            const auto* aset = s.world_ctx->world->ability_sets.get(carrier_id);
            const simulation::AbilityInstance* inst = nullptr;
            if (aset) {
                for (const auto& a : aset->abilities) {
                    if (a.ability_id == fa) { inst = &a; break; }
                }
            }
            const auto* abil_def = s.world_ctx->abilities->get(fa);
            if (inst && abil_def && is_castable_form(abil_def->form)) {
                bool on_cooldown = inst->cooldown_remaining > 0.05f
                                && abil_def->level_data(inst->level).cooldown > 0.0f;
                if (on_cooldown) {
                    f32 total = abil_def->level_data(inst->level).cooldown;
                    f32 frac  = inst->cooldown_remaining / total;
                    draw_cooldown_pie(r, icon_rect, frac, cfg.style.cooldown_overlay);

                    char buf[16];
                    format_cooldown_secs(inst->cooldown_remaining, buf, sizeof(buf));
                    if (buf[0] != '\0') {
                        f32 px = cfg.style.cooldown_text_size;
                        f32 tw = measure_text(r, buf, px);
                        f32 line_h = measure_line_height(r, px);
                        f32 ascent = measure_ascent(r, px);
                        f32 tx = icon_rect.x + (icon_rect.w - tw) * 0.5f;
                        f32 ty = icon_rect.y + (icon_rect.h - line_h) * 0.5f + ascent;
                        emit_text(r, tx, ty, buf, cfg.style.cooldown_text_color, px);
                    }
                } else if (!can_afford(*s.world_ctx->world, carrier_id,
                                       abil_def->level_data(inst->level))) {
                    emit_rect(r, icon_rect, cfg.style.disabled_tint);
                }
            }
        }

        if (bw > 0.0f && (slot.style.border_color.rgba >> 24) != 0) {
            Rect rc = slot.rect;
            emit_rect(r, { rc.x, rc.y, rc.w, bw }, slot.style.border_color);
            emit_rect(r, { rc.x, rc.y + rc.h - bw, rc.w, bw }, slot.style.border_color);
            emit_rect(r, { rc.x, rc.y, bw, rc.h }, slot.style.border_color);
            emit_rect(r, { rc.x + rc.w - bw, rc.y, bw, rc.h }, slot.style.border_color);
        }

        if (has_item && !slot.hotkey.empty()) {
            f32 px_size = slot.rect.h * 0.28f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = measure_text(r, slot.hotkey, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 ascent = measure_ascent(r, px_size);
            f32 y_base = slot.rect.y + ascent + 2.0f;
            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.hotkey_badge_bg);
            }
            emit_text(r, x_left, y_base, slot.hotkey, cfg.style.hotkey_color, px_size);
        }

        if (info && info->charges > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", info->charges);
            f32 px = cfg.style.charges_text_size;
            f32 tw = measure_text(r, buf, px);
            f32 ascent = measure_ascent(r, px);
            f32 x_left = slot.rect.x + slot.rect.w - tw - 4.0f;
            f32 y_base = slot.rect.y + slot.rect.h - 4.0f;
            if ((cfg.style.charges_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    tw + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.charges_badge_bg);
            }
            emit_text(r, x_left, y_base, buf, cfg.style.charges_color, px);
        }

        if (info && info->level > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", info->level);
            f32 px = cfg.style.level_text_size;
            f32 tw = measure_text(r, buf, px);
            f32 ascent = measure_ascent(r, px);
            f32 x_left = slot.rect.x + 4.0f;
            f32 y_base = slot.rect.y + ascent + 2.0f;
            if ((cfg.style.level_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    tw + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                emit_rect(r, bg_pill, cfg.style.level_badge_bg);
            }
            emit_text(r, x_left, y_base, buf, cfg.style.level_color, px);
        }
    }
}

// ── Display message ──────────────────────────────────────────────────────

static void draw_display_message(HudRenderer::Impl& r, Hud::Impl& s) {
    const auto& cfg = s.display_message_cfg;
    const auto& rt  = s.display_message_rt;
    if (!cfg.enabled || !rt.visible || rt.lines.empty()) return;

    const auto& style = cfg.style;
    if ((style.bg.rgba >> 24) != 0) emit_rect(r, cfg.rect, style.bg);

    f32 line_h = measure_line_height(r, style.text_size);
    f32 ascent = measure_ascent(r, style.text_size);
    f32 cursor_y = cfg.rect.y + 4.0f;

    for (const auto& line : rt.lines) {
        if (s.local_player != UINT32_MAX
            && !(line.players_mask & (1u << s.local_player))) {
            continue;
        }
        std::string text;
        if (s.locale_manager) text = s.locale_manager->resolve(i18n::Pool::Map, line.loc);
        else                  text = line.loc.key;
        if (text.empty()) { cursor_y += line_h; continue; }

        f32 alpha = 1.0f;
        if (line.fadepoint > 0.0f) {
            f32 fade_start = line.lifespan - line.fadepoint;
            if (line.age > fade_start) {
                alpha = 1.0f - (line.age - fade_start) / line.fadepoint;
                if (alpha < 0.0f) alpha = 0.0f;
            }
        }
        Color c = style.text_color;
        u32 base_a = (c.rgba >> 24) & 0xFFu;
        u32 a = static_cast<u32>(static_cast<f32>(base_a) * alpha);
        c.rgba = (c.rgba & 0x00FFFFFFu) | (a << 24);

        emit_text(r, cfg.rect.x + 4.0f, cursor_y + ascent, text, c, style.text_size);
        cursor_y += line_h;
    }
}

// ── Tooltip ──────────────────────────────────────────────────────────────

static void tooltip_keys(const Hud::Impl& s,
                         i18n::LocalizedString& out_name,
                         i18n::LocalizedString& out_body) {
    out_name.clear();
    out_body.clear();
    switch (s.tooltip.source) {
        case Hud::Impl::TooltipState::Source::ActionBar: {
            if (!s.world_ctx) return;
            const simulation::AbilityDef* def = nullptr;
            resolve_slot_ability(static_cast<u32>(s.tooltip.slot_index),
                                 s.action_bar_cfg, *s.world_ctx, def);
            if (!def) return;
            out_name.key = "ability." + def->id + ".name";
            out_body.key = "ability." + def->id + ".tooltip";
            break;
        }
        case Hud::Impl::TooltipState::Source::Inventory: {
            const simulation::Inventory* inv = inventory_resolve_selected(s);
            simulation::Item item;
            const simulation::ItemInfo*    info = nullptr;
            const simulation::ItemTypeDef* def  = nullptr;
            if (!inventory_resolve_slot(s, inv,
                                        static_cast<u32>(s.tooltip.slot_index),
                                        item, info, def)) return;
            if (!def) return;
            out_name.key = "item." + def->id + ".name";
            out_body.key = "item." + def->id + ".tooltip";
            break;
        }
        case Hud::Impl::TooltipState::Source::CommandBar: {
            const auto& slots = s.command_bar_cfg.slots;
            u32 idx = static_cast<u32>(s.tooltip.slot_index);
            if (idx >= slots.size()) return;
            const std::string& cmd = slots[idx].command;
            if (cmd.empty()) return;
            out_name.key = "ui.command." + cmd + ".name";
            out_body.key = "ui.command." + cmd + ".tooltip";
            break;
        }
        default: break;
    }
}

static std::vector<std::string> tooltip_wrap(const HudRenderer::Impl& r,
                                             const std::string& text,
                                             f32 px_size, f32 max_w) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    size_t i = 0;
    while (i < text.size()) {
        size_t line_end = text.find('\n', i);
        std::string segment = (line_end == std::string::npos)
                                ? text.substr(i)
                                : text.substr(i, line_end - i);
        i = (line_end == std::string::npos) ? text.size() : (line_end + 1);

        std::string current;
        size_t k = 0;
        while (k < segment.size()) {
            size_t ws = k;
            while (ws < segment.size() && segment[ws] == ' ') ++ws;
            std::string spaces = segment.substr(k, ws - k);
            size_t we = ws;
            while (we < segment.size() && segment[we] != ' ') ++we;
            std::string word = segment.substr(ws, we - ws);
            k = we;

            std::string candidate = current + spaces + word;
            f32 cw = measure_text(r, candidate, px_size);
            if (cw <= max_w || current.empty()) {
                current = std::move(candidate);
            } else {
                out.push_back(std::move(current));
                current = word;
            }
            if (measure_text(r, current, px_size) > max_w) {
                std::string acc;
                size_t cp = 0;
                while (cp < current.size()) {
                    unsigned char c = static_cast<unsigned char>(current[cp]);
                    size_t n = 1;
                    if      ((c & 0x80) == 0x00) n = 1;
                    else if ((c & 0xE0) == 0xC0) n = 2;
                    else if ((c & 0xF0) == 0xE0) n = 3;
                    else if ((c & 0xF8) == 0xF0) n = 4;
                    if (cp + n > current.size()) n = current.size() - cp;
                    std::string trial = acc + current.substr(cp, n);
                    if (measure_text(r, trial, px_size) > max_w && !acc.empty()) {
                        out.push_back(std::move(acc));
                        acc.clear();
                    }
                    acc += current.substr(cp, n);
                    cp  += n;
                }
                current = std::move(acc);
            }
        }
        out.push_back(std::move(current));
    }
    return out;
}

static void draw_tooltip(HudRenderer::Impl& r, Hud::Impl& s) {
    using TT = Hud::Impl::TooltipState;
    if (s.tooltip.source == TT::Source::None) return;
    auto now = std::chrono::steady_clock::now();
    if (now < s.tooltip.activate_at) return;
    s.tooltip.visible = true;

    Rect anchor{};
    bool have_anchor = false;
    u32 idx = static_cast<u32>(s.tooltip.slot_index);
    if (s.tooltip.source == TT::Source::ActionBar
        && idx < s.action_bar_cfg.slots.size()) {
        anchor = s.action_bar_cfg.slots[idx].rect;
        have_anchor = true;
    } else if (s.tooltip.source == TT::Source::Inventory
               && idx < s.inventory_cfg.slots.size()) {
        anchor = s.inventory_cfg.slots[idx].rect;
        have_anchor = true;
    } else if (s.tooltip.source == TT::Source::CommandBar
               && idx < s.command_bar_cfg.slots.size()) {
        anchor = s.command_bar_cfg.slots[idx].rect;
        have_anchor = true;
    }
    if (!have_anchor) return;

    auto resolve = [&](const i18n::LocalizedString& l) -> std::string {
        if (l.empty()) return {};
        if (!s.locale_manager) return l.key;
        return s.locale_manager->resolve(i18n::Pool::Map, l);
    };
    i18n::LocalizedString name_loc, body_loc;
    tooltip_keys(s, name_loc, body_loc);

    constexpr f32 PAD     = 8.0f;
    constexpr f32 GAP     = 4.0f;
    constexpr f32 MAX_W   = 280.0f;
    constexpr f32 NAME_PX = 15.0f;
    constexpr f32 BODY_PX = 12.0f;

    struct Line { std::string text; f32 px; Color color; };
    std::vector<Line> lines;
    if (auto t = resolve(name_loc); !t.empty()) {
        lines.push_back({ std::move(t), NAME_PX, rgba(255, 230, 150, 255) });
    }
    for (auto& wrapped : tooltip_wrap(r, resolve(body_loc), BODY_PX, MAX_W - PAD * 2.0f)) {
        lines.push_back({ std::move(wrapped), BODY_PX, rgba(220, 220, 220, 255) });
    }
    if (lines.empty()) return;

    f32 content_w = 0.0f;
    f32 panel_h   = PAD * 2.0f;
    for (size_t li = 0; li < lines.size(); ++li) {
        if (li > 0 && lines[li - 1].px != lines[li].px) panel_h += GAP;
        panel_h   += measure_line_height(r, lines[li].px);
        content_w  = std::max(content_w, measure_text(r, lines[li].text, lines[li].px));
    }
    f32 panel_w = std::min(MAX_W, content_w + PAD * 2.0f);

    f32 px = anchor.x + anchor.w * 0.5f - panel_w * 0.5f;
    f32 py = anchor.y - panel_h - 8.0f;
    if (py < 4.0f) py = anchor.y + anchor.h + 8.0f;
    f32 sw = static_cast<f32>(s.screen_w);
    if (px < 4.0f) px = 4.0f;
    if (px + panel_w > sw - 4.0f) px = sw - 4.0f - panel_w;

    Color bg     = rgba(20, 22, 30, 235);
    Color border = rgba(255, 255, 255, 90);
    emit_rect(r, { px, py, panel_w, panel_h }, bg);
    emit_rect(r, { px, py, panel_w, 1.0f }, border);
    emit_rect(r, { px, py + panel_h - 1.0f, panel_w, 1.0f }, border);
    emit_rect(r, { px, py, 1.0f, panel_h }, border);
    emit_rect(r, { px + panel_w - 1.0f, py, 1.0f, panel_h }, border);

    f32 cursor_y = py + PAD;
    for (size_t li = 0; li < lines.size(); ++li) {
        if (li > 0 && lines[li - 1].px != lines[li].px) cursor_y += GAP;
        f32 baseline = cursor_y + measure_ascent(r, lines[li].px);
        emit_text(r, px + PAD, baseline, lines[li].text, lines[li].color, lines[li].px);
        cursor_y += measure_line_height(r, lines[li].px);
    }
}

// ── Text tags (world-anchored floating text) ─────────────────────────────

static bool project_world_for_tag(const glm::mat4& vp, const glm::vec3& world,
                                  u32 screen_w, u32 screen_h,
                                  f32& sx, f32& sy) {
    glm::vec4 clip = vp * glm::vec4(world, 1.0f);
    if (clip.w <= 0.001f) return false;
    f32 ndc_x = clip.x / clip.w;
    f32 ndc_y = clip.y / clip.w;
    sx = (ndc_x * 0.5f + 0.5f) * static_cast<f32>(screen_w);
    sy = (ndc_y * 0.5f + 0.5f) * static_cast<f32>(screen_h);
    return true;
}

static void draw_text_tags(HudRenderer::Impl& r, Hud::Impl& s,
                           const WorldContext& ctx, f32 alpha) {
    if (!ctx.camera || !ctx.world) return;
    const glm::mat4 vp = ctx.camera->view_projection();
    const auto& world  = *ctx.world;
    const u32 sw = s.screen_w, sh = s.screen_h;
    for (auto& t : s.text_tags) {
        if (!t.alive || !t.visible || t.text.empty()) continue;
        if (!(t.players_mask & (1u << s.local_player))) continue;

        std::string rendered = s.locale_manager
            ? s.locale_manager->resolve(i18n::Pool::Map, t.text)
            : t.text.key;
        if (rendered.empty()) continue;

        glm::vec3 world_anchor{0.0f};
        if (t.unit_id != UINT32_MAX) {
            const auto* tf = world.transforms.get(t.unit_id);
            if (!tf) continue;
            world_anchor = tf->interp_position(alpha) + glm::vec3(0.0f, 0.0f, t.z_offset);
        } else {
            world_anchor = t.world_pos + glm::vec3(0.0f, 0.0f, t.z_offset);
        }

        f32 cx = 0.0f, cy = 0.0f;
        if (!project_world_for_tag(vp, world_anchor, sw, sh, cx, cy)) continue;

        cx += t.screen_dx;
        cy += t.screen_dy;

        f32 fade = 1.0f;
        if (t.lifespan > 0.0f && t.fadepoint > 0.0f) {
            f32 fade_start = t.lifespan - t.fadepoint;
            if (t.age > fade_start) {
                f32 f = 1.0f - (t.age - fade_start) / t.fadepoint;
                if (f < 0.0f) f = 0.0f;
                fade = f;
            }
        }

        u8 base_a = static_cast<u8>((t.color.rgba >> 24) & 0xFF);
        u8 out_a  = static_cast<u8>(base_a * fade);
        Color final_color{ (t.color.rgba & 0x00FFFFFFu) | (static_cast<u32>(out_a) << 24) };

        f32 text_w    = measure_text(r, rendered, t.px_size);
        f32 line_h    = measure_line_height(r, t.px_size);
        f32 ascent    = measure_ascent(r, t.px_size);
        f32 x_left    = cx - text_w * 0.5f;
        f32 y_baseline = cy + ascent - line_h * 0.5f;
        emit_text(r, x_left, y_baseline, rendered, final_color, t.px_size);
    }
}

// ── HudRenderer methods ──────────────────────────────────────────────────

HudRenderer::HudRenderer() = default;

HudRenderer::~HudRenderer() {
    shutdown();
}

bool HudRenderer::init(Hud& hud, rhi::Rhi& rhi) {
    if (m_impl) {
        log::error(TAG, "HudRenderer::init called twice");
        return false;
    }
    m_hud  = &hud;
    m_impl = new Impl{};
    auto& r = *m_impl;
    r.rhi = &rhi;
    r.verts.reserve(MAX_VERTS);
    r.inds.reserve(MAX_INDS);

    if (!create_descriptor_layout(r)) { log::error(TAG, "desc layout create failed");   return false; }
    if (!create_sampler(r))           { log::error(TAG, "sampler create failed");       return false; }
    if (!create_pipeline_layout(r))   { log::error(TAG, "pipeline layout create failed"); return false; }
    if (!create_pipeline_variant(r, "engine/shaders/hud.frag.spv", r.pipe_solid)) {
        log::error(TAG, "solid pipeline create failed"); return false;
    }
    if (!create_pipeline_variant(r, "engine/shaders/hud_text.frag.spv", r.pipe_text)) {
        log::error(TAG, "text pipeline create failed"); return false;
    }
    if (!create_ring_buffers(r))      { log::error(TAG, "ring buffer create failed");   return false; }
    if (!create_white_texture(r))     { log::error(TAG, "white texture create failed"); return false; }

    r.font = std::make_unique<Font>();
    r.font->init_from_system(rhi, r.desc_layout, r.sampler);

    log::info(TAG, "HudRenderer initialized");
    return true;
}

void HudRenderer::shutdown() {
    if (!m_impl) return;
    auto& r = *m_impl;
    if (r.font) { r.font->shutdown(); r.font.reset(); }
    if (r.rhi) {
        VkDevice device = r.rhi->device();
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            for (auto& ring : r.rings) {
                r.rhi->destroy_buffer(ring.vb);
                r.rhi->destroy_buffer(ring.ib);
            }
            for (auto& [path, img] : r.images) {
                if (!img) continue;
                if (img->set.is_valid()) r.rhi->free_descriptor_set(img->set);
                r.rhi->destroy_texture(img->handle);
            }
            r.images.clear();
            if (r.white_set.is_valid()) r.rhi->free_descriptor_set(r.white_set);
            r.rhi->destroy_texture(r.white_image);
            r.rhi->destroy_pipeline(r.pipe_text);
            r.rhi->destroy_pipeline(r.pipe_solid);
            r.rhi->destroy_pipeline_layout(r.pipe_layout);
            r.rhi->destroy_sampler(r.sampler);
            r.rhi->destroy_descriptor_set_layout(r.desc_layout);
        }
    }
    delete m_impl;
    m_impl = nullptr;
    m_hud  = nullptr;
}

void HudRenderer::begin_frame(u32 screen_w, u32 screen_h) {
    if (!m_impl || !m_hud) return;
    auto& r = *m_impl;
    r.verts.clear();
    r.inds.clear();
    r.batches.clear();

    auto& s = *m_hud->impl();
    f32 sc = s.ui_scale;
    s.physical_w = screen_w;
    s.physical_h = screen_h;
    s.screen_w   = static_cast<u32>(static_cast<f32>(screen_w) / sc);
    s.screen_h   = static_cast<u32>(static_cast<f32>(screen_h) / sc);
    r.frame_open = true;
    if (s.root) {
        s.root->rect = { 0.0f, 0.0f,
                         static_cast<f32>(s.screen_w),
                         static_cast<f32>(s.screen_h) };
    }
}

void HudRenderer::draw_rect(const Rect& rc, Color color) {
    if (!m_impl) return;
    emit_rect(*m_impl, rc, color);
}

void HudRenderer::draw_marquee(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (!m_impl || !m_hud) return;
    auto& r = *m_impl;
    if (!r.frame_open) return;
    f32 xa = std::min(x0, x1), xb = std::max(x0, x1);
    f32 ya = std::min(y0, y1), yb = std::max(y0, y1);
    Rect rc{ xa, ya, xb - xa, yb - ya };
    if (rc.w <= 0.0f || rc.h <= 0.0f) return;

    const auto& style = m_hud->marquee_style();
    if ((style.fill.rgba >> 24) != 0) emit_rect(r, rc, style.fill);
    if ((style.border.rgba >> 24) != 0) {
        emit_rect(r, { rc.x, rc.y, rc.w, 1.0f },               style.border);
        emit_rect(r, { rc.x, rc.y + rc.h - 1.0f, rc.w, 1.0f }, style.border);
        emit_rect(r, { rc.x, rc.y, 1.0f, rc.h },               style.border);
        emit_rect(r, { rc.x + rc.w - 1.0f, rc.y, 1.0f, rc.h }, style.border);
    }
}

void HudRenderer::draw_image(const Rect& rc, std::string_view asset_path, Color tint) {
    if (!m_impl) return;
    emit_image(*m_impl, rc, asset_path, tint);
}

void HudRenderer::draw_image_disc(f32 cx, f32 cy, f32 radius,
                                  std::string_view asset_path, Color tint) {
    if (!m_impl) return;
    emit_image_disc(*m_impl, cx, cy, radius, asset_path, tint);
}

void HudRenderer::draw_text(f32 x_left, f32 y_baseline, std::string_view utf8,
                            Color color, f32 px_size) {
    if (!m_impl) return;
    emit_text(*m_impl, x_left, y_baseline, utf8, color, px_size);
}

f32 HudRenderer::text_ascent_px(f32 px_size) const {
    return m_impl ? measure_ascent(*m_impl, px_size) : 0.0f;
}

f32 HudRenderer::text_line_height_px(f32 px_size) const {
    return m_impl ? measure_line_height(*m_impl, px_size) : 0.0f;
}

f32 HudRenderer::text_width_px(std::string_view utf8, f32 px_size) const {
    return m_impl ? measure_text(*m_impl, utf8, px_size) : 0.0f;
}

void HudRenderer::draw_tree() {
    if (!m_impl || !m_hud) return;
    auto& r = *m_impl;
    if (!r.frame_open) return;
    auto& s = *m_hud->impl();
    if (!s.root) return;

    s.root->draw(*this);
    draw_action_bar(r, s);
    draw_command_bar(r, s);
    draw_inventory(r, s);
    draw_display_message(r, s);
    draw_minimap(r, s);
    draw_joystick(r, s);

    if (!s.is_mobile) {
        const auto& style = s.cast_indicator_cfg.style;
        bool targeting = m_hud->aim_state().active;
        const std::string& path = targeting ? style.cursor_target_path
                                            : style.cursor_default_path;
        if (!path.empty()) {
            f32 size = (style.cursor_size > 0.0f) ? style.cursor_size : 20.0f;
            f32 ax = targeting ? size * 0.5f : 0.0f;
            f32 ay = targeting ? size * 0.5f : 0.0f;
            f32 cx = s.pointer_x;
            f32 cy = s.pointer_y;
            Rect rc{ cx - ax, cy - ay, size, size };
            Color tint = style.intents.neutral;
            switch (m_hud->cursor_intent()) {
                case Hud::TargetingIntent::Enemy: tint = style.intents.enemy; break;
                case Hud::TargetingIntent::Ally:  tint = style.intents.ally;  break;
                case Hud::TargetingIntent::Item:  tint = style.intents.item;  break;
                default:                          tint = style.intents.neutral; break;
            }
            emit_image(r, rc, path, tint);
        }
    }

    if (s.held_item_slot >= 0 && !s.held_item_icon.empty()) {
        constexpr f32 kHeldIconSize = 28.0f;
        f32 cx = s.pointer_x;
        f32 cy = s.pointer_y;
        Rect rc{ cx - kHeldIconSize * 0.5f, cy - kHeldIconSize * 0.5f,
                 kHeldIconSize, kHeldIconSize };
        emit_image(r, rc, s.held_item_icon, rgba(255, 255, 255, 220));
    }

    if (s.held_item_slot < 0) {
        draw_tooltip(r, s);
    }
}

// Forward decls — implementations live in src/render/hud/world.cpp.
void draw_entity_bars_impl(HudRenderer& renderer,
                           u32 screen_w, u32 screen_h,
                           const WorldOverlayConfig& cfg,
                           const WorldContext& ctx,
                           f32 alpha);
void draw_unit_name_label_impl(HudRenderer& renderer,
                               u32 screen_w, u32 screen_h,
                               const WorldOverlayConfig& cfg,
                               const WorldContext& ctx,
                               f32 alpha);

void HudRenderer::draw_world_overlays(f32 alpha) {
    if (!m_impl || !m_hud) return;
    auto& r = *m_impl;
    if (!r.frame_open) return;
    auto& s = *m_hud->impl();
    if (!s.world_ctx) return;
    draw_entity_bars_impl(*this, s.screen_w, s.screen_h, s.world_cfg, *s.world_ctx, alpha);
    draw_unit_name_label_impl(*this, s.screen_w, s.screen_h, s.world_cfg, *s.world_ctx, alpha);
    draw_text_tags(r, s, *s.world_ctx, alpha);
}

void HudRenderer::render(rhi::CommandList& cmd) {
    if (!m_impl || !m_hud) return;
    auto& r = *m_impl;
    if (!r.frame_open) return;
    r.frame_open = false;

    auto& s = *m_hud->impl();
    if (r.verts.empty() || r.inds.empty() || r.batches.empty()) return;
    if (s.screen_w == 0 || s.screen_h == 0) return;

    r.batches.back().index_count = static_cast<u32>(r.inds.size()) - r.batches.back().index_start;

    u32 slot = r.rhi->frame_index();
    RingBuffer& ring = r.rings[slot];
    std::memcpy(r.rhi->mapped_ptr(ring.vb), r.verts.data(), r.verts.size() * sizeof(Vertex));
    std::memcpy(r.rhi->mapped_ptr(ring.ib), r.inds.data(),  r.inds.size()  * sizeof(u16));

    cmd.set_viewport(0, 0, static_cast<f32>(s.physical_w), static_cast<f32>(s.physical_h));
    cmd.set_scissor(0, 0, s.physical_w, s.physical_h);

    glm::mat4 mvp = glm::ortho(0.0f, static_cast<f32>(s.screen_w),
                               0.0f, static_cast<f32>(s.screen_h),
                               -1.0f, 1.0f);
    cmd.push_constants(r.pipe_layout, rhi::ShaderStage::Vertex,
                       0, sizeof(glm::mat4), glm::value_ptr(mvp));

    cmd.bind_vertex_buffer(0, ring.vb);
    cmd.bind_index_buffer(ring.ib, 0, rhi::IndexType::U16);

    rhi::PipelineHandle      last_pipe{};
    rhi::DescriptorSetHandle last_set{};
    for (const Batch& b : r.batches) {
        if (b.index_count == 0) continue;
        rhi::PipelineHandle pipe = (b.pipeline == PIPE_TEXT) ? r.pipe_text : r.pipe_solid;
        if (pipe != last_pipe) {
            cmd.bind_pipeline(pipe);
            last_pipe = pipe;
        }
        if (b.desc_set != last_set) {
            cmd.bind_descriptor_set(r.pipe_layout, 0, b.desc_set);
            last_set = b.desc_set;
        }
        cmd.draw_indexed(b.index_count, 1, b.index_start, 0, 0);
    }
}

void HudRenderer::reset_session_images() {
    if (!m_impl) return;
    auto& r = *m_impl;
    if (r.rhi) r.rhi->wait_idle();
    destroy_hud_images(r);
}

} // namespace uldum::hud

