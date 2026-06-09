#include "render/gpu_texture.h"
#include "rhi/rhi.h"
#include "asset/texture.h"
#include "core/log.h"

#include <cstring>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "GpuTexture";

namespace {

// One-shot UNDEFINED → TRANSFER_DST → copy → SHADER_READ_ONLY transition
// shared by upload_texture_rgba / _array / _cubemap. `regions` describes
// which buffer offsets feed which image layers/mips. `base_layer` is the
// first array layer the barriers cover — 0 for whole-image uploads, or a
// specific layer when streaming a single slice into an existing array
// (see upload_array_layer); the copy regions still carry their own
// base_array_layer.
void upload_pixels_to_image(rhi::Rhi& rhi, rhi::TextureHandle image, u32 layer_count,
                            rhi::BufferHandle staging,
                            std::span<const rhi::BufferImageCopy> regions,
                            u32 base_layer = 0) {
    rhi::CommandList cmd = rhi.begin_oneshot();

    rhi::ImageBarrier to_xfer{};
    to_xfer.image       = image;
    to_xfer.src_stage   = rhi::PipelineStage::TopOfPipe;
    to_xfer.dst_stage   = rhi::PipelineStage::Transfer;
    to_xfer.dst_access  = rhi::AccessFlag::TransferWrite;
    to_xfer.old_layout  = rhi::ImageLayout::Undefined;
    to_xfer.new_layout  = rhi::ImageLayout::TransferDstOptimal;
    to_xfer.base_layer  = base_layer;
    to_xfer.layer_count = layer_count;
    cmd.image_barrier(to_xfer);

    cmd.copy_buffer_to_image(staging, image, regions);

    rhi::ImageBarrier to_shader{};
    to_shader.image       = image;
    to_shader.src_stage   = rhi::PipelineStage::Transfer;
    to_shader.src_access  = rhi::AccessFlag::TransferWrite;
    to_shader.dst_stage   = rhi::PipelineStage::FragmentShader;
    to_shader.dst_access  = rhi::AccessFlag::ShaderRead;
    to_shader.old_layout  = rhi::ImageLayout::TransferDstOptimal;
    to_shader.new_layout  = rhi::ImageLayout::ShaderReadOnlyOptimal;
    to_shader.base_layer  = base_layer;
    to_shader.layer_count = layer_count;
    cmd.image_barrier(to_shader);

    rhi.end_oneshot(cmd);
}

rhi::BufferHandle make_staging(rhi::Rhi& rhi, u64 size) {
    rhi::BufferDesc d{};
    d.size   = size;
    d.usage  = rhi::BufferUsage::TransferSrc;
    d.memory = rhi::MemoryUsage::HostSequential;
    return rhi.create_buffer(d);
}

} // namespace

GpuTexture upload_texture_rgba(rhi::Rhi& rhi, const u8* pixels, u32 width, u32 height, bool srgb, bool clamp) {
    u64 image_size = static_cast<u64>(width) * height * 4;

    auto staging = make_staging(rhi, image_size);
    if (!staging.is_valid()) {
        log::error(TAG, "Failed to create staging buffer");
        return {};
    }
    std::memcpy(rhi.mapped_ptr(staging), pixels, image_size);

    GpuTexture tex{};
    tex.width  = width;
    tex.height = height;

    rhi::TextureDesc td{};
    td.width  = width;
    td.height = height;
    td.format = srgb ? rhi::TextureFormat::R8G8B8A8_SRGB : rhi::TextureFormat::R8G8B8A8_UNORM;
    td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    tex.texture = rhi.create_texture(td);
    if (!tex.texture.is_valid()) {
        log::error(TAG, "Failed to create GPU image");
        rhi.destroy_buffer(staging);
        return {};
    }

    rhi::BufferImageCopy region{};
    region.image_extent_w = width;
    region.image_extent_h = height;
    upload_pixels_to_image(rhi, tex.texture, 1, staging, std::span{&region, 1});

    rhi.destroy_buffer(staging);

    rhi::SamplerDesc sd{};
    sd.address_u = clamp ? rhi::AddressMode::ClampToEdge : rhi::AddressMode::Repeat;
    sd.address_v = sd.address_u;
    sd.address_w = sd.address_u;
    sd.max_lod   = 0.0f;
    tex.sampler  = rhi.create_sampler(sd);
    if (!tex.sampler.is_valid()) {
        rhi.destroy_texture(tex.texture);
        return {};
    }

    log::info(TAG, "Uploaded texture {}x{}", width, height);
    return tex;
}

GpuTexture upload_texture(rhi::Rhi& rhi, const asset::TextureData& data) {
    if (data.pixels.empty() || data.width == 0 || data.height == 0) {
        log::error(TAG, "Invalid texture data");
        return {};
    }
    if (data.channels == 4) {
        return upload_texture_rgba(rhi, data.pixels.data(), data.width, data.height);
    }
    // Pad to RGBA for non-4-channel sources.
    std::vector<u8> rgba(static_cast<usize>(data.width) * data.height * 4);
    for (u32 y = 0; y < data.height; ++y) {
        for (u32 x = 0; x < data.width; ++x) {
            usize i_src = (static_cast<usize>(y) * data.width + x) * data.channels;
            usize i_dst = (static_cast<usize>(y) * data.width + x) * 4;
            rgba[i_dst + 0] = data.channels >= 1 ? data.pixels[i_src + 0] : 0;
            rgba[i_dst + 1] = data.channels >= 2 ? data.pixels[i_src + 1] : 0;
            rgba[i_dst + 2] = data.channels >= 3 ? data.pixels[i_src + 2] : 0;
            rgba[i_dst + 3] = data.channels >= 4 ? data.pixels[i_src + 3] : 255;
        }
    }
    return upload_texture_rgba(rhi, rgba.data(), data.width, data.height);
}

GpuTexture upload_texture_array(rhi::Rhi& rhi, const u8** layers_data, u32 layer_count,
                                u32 width, u32 height, bool srgb) {
    u64 layer_size = static_cast<u64>(width) * height * 4;
    u64 total_size = layer_size * layer_count;

    auto staging = make_staging(rhi, total_size);
    if (!staging.is_valid()) {
        log::error(TAG, "Failed to create array texture staging buffer");
        return {};
    }
    auto* mapped = static_cast<u8*>(rhi.mapped_ptr(staging));
    for (u32 i = 0; i < layer_count; ++i) {
        std::memcpy(mapped + i * layer_size, layers_data[i], layer_size);
    }

    GpuTexture tex{};
    tex.width  = width;
    tex.height = height;

    rhi::TextureDesc td{};
    td.width        = width;
    td.height       = height;
    td.array_layers = layer_count;
    td.format       = srgb ? rhi::TextureFormat::R8G8B8A8_SRGB : rhi::TextureFormat::R8G8B8A8_UNORM;
    td.usage        = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    tex.texture = rhi.create_texture(td);
    if (!tex.texture.is_valid()) {
        log::error(TAG, "Failed to create array texture image");
        rhi.destroy_buffer(staging);
        return {};
    }

    std::vector<rhi::BufferImageCopy> regions(layer_count);
    for (u32 i = 0; i < layer_count; ++i) {
        regions[i].buffer_offset    = i * layer_size;
        regions[i].base_array_layer = i;
        regions[i].image_extent_w   = width;
        regions[i].image_extent_h   = height;
    }
    upload_pixels_to_image(rhi, tex.texture, layer_count, staging, std::span{regions});

    rhi.destroy_buffer(staging);

    {
        rhi::SamplerDesc sd{};
        sd.max_lod = 0.0f;
        tex.sampler = rhi.create_sampler(sd);
        if (!tex.sampler.is_valid()) {
            rhi.destroy_texture(tex.texture);
            return {};
        }
    }

    log::info(TAG, "Uploaded texture array {}x{} x {} layers", width, height, layer_count);
    return tex;
}

GpuTexture upload_texture_cubemap(rhi::Rhi& rhi, const u8* faces[6],
                                  u32 width, u32 height) {
    u64 face_size  = static_cast<u64>(width) * height * 4;
    u64 total_size = face_size * 6;

    auto staging = make_staging(rhi, total_size);
    if (!staging.is_valid()) {
        log::error(TAG, "Failed to create cubemap staging buffer");
        return {};
    }
    auto* mapped = static_cast<u8*>(rhi.mapped_ptr(staging));
    for (u32 i = 0; i < 6; ++i) {
        std::memcpy(mapped + i * face_size, faces[i], face_size);
    }

    GpuTexture tex{};
    tex.width  = width;
    tex.height = height;

    rhi::TextureDesc td{};
    td.width        = width;
    td.height       = height;
    td.array_layers = 6;
    td.type         = rhi::TextureType::TextureCube;
    td.format       = rhi::TextureFormat::R8G8B8A8_SRGB;
    td.usage        = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    tex.texture = rhi.create_texture(td);
    if (!tex.texture.is_valid()) {
        log::error(TAG, "Failed to create cubemap image");
        rhi.destroy_buffer(staging);
        return {};
    }

    rhi::BufferImageCopy regions[6]{};
    for (u32 i = 0; i < 6; ++i) {
        regions[i].buffer_offset    = i * face_size;
        regions[i].base_array_layer = i;
        regions[i].image_extent_w   = width;
        regions[i].image_extent_h   = height;
    }
    upload_pixels_to_image(rhi, tex.texture, 6, staging, std::span{regions, 6});

    rhi.destroy_buffer(staging);

    {
        rhi::SamplerDesc sd{};
        sd.address_u = rhi::AddressMode::ClampToEdge;
        sd.address_v = rhi::AddressMode::ClampToEdge;
        sd.address_w = rhi::AddressMode::ClampToEdge;
        sd.max_lod   = 0.0f;
        tex.sampler  = rhi.create_sampler(sd);
        if (!tex.sampler.is_valid()) {
            rhi.destroy_texture(tex.texture);
            return {};
        }
    }

    log::info(TAG, "Uploaded cubemap {}x{}", width, height);
    return tex;
}

void destroy_texture(rhi::Rhi& rhi, GpuTexture& tex) {
    rhi.destroy_sampler(tex.sampler);
    rhi.destroy_texture(tex.texture);
    tex = {};
}

bool upload_array_layer(rhi::Rhi& rhi, rhi::TextureHandle array_tex, u32 layer,
                        const u8* pixels, u32 width, u32 height) {
    if (!array_tex.is_valid() || !pixels || width == 0 || height == 0) return false;

    const u64 layer_bytes = static_cast<u64>(width) * height * 4;
    auto staging = make_staging(rhi, layer_bytes);
    if (!staging.is_valid()) {
        log::error(TAG, "upload_array_layer: staging buffer alloc failed");
        return false;
    }
    std::memcpy(rhi.mapped_ptr(staging), pixels, layer_bytes);

    rhi::BufferImageCopy region{};
    region.base_array_layer = layer;
    region.image_extent_w   = width;
    region.image_extent_h   = height;
    upload_pixels_to_image(rhi, array_tex, 1, staging, std::span{&region, 1}, layer);

    rhi.destroy_buffer(staging);
    return true;
}

} // namespace uldum::render
