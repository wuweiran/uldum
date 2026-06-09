#pragma once

#include "core/types.h"
#include "rhi/handles.h"

namespace uldum::asset { struct TextureData; }
namespace uldum::rhi   { class Rhi; }

namespace uldum::render {

struct GpuTexture {
    rhi::TextureHandle texture{};
    rhi::SamplerHandle sampler{};
    u32                width  = 0;
    u32                height = 0;
};

// Upload CPU texture data to GPU. Creates image, view, and sampler.
GpuTexture upload_texture(rhi::Rhi& rhi, const asset::TextureData& tex);

// Upload raw RGBA pixel data to GPU. `srgb` is true for color textures
// (default — image bytes interpreted as sRGB, gamma-decoded on sample);
// pass false for data textures (alpha masks, normal maps) so the bytes
// are sampled linearly without gamma decode. `clamp` switches the
// sampler address mode from REPEAT to CLAMP_TO_EDGE — needed for
// decals whose UV ramp shouldn't wrap (e.g. curve textures with a
// V-axis alpha gradient).
GpuTexture upload_texture_rgba(rhi::Rhi& rhi, const u8* pixels, u32 width, u32 height,
                               bool srgb = true, bool clamp = false);

// Upload an array of RGBA layers as a sampler2DArray. Each layer must be width*height*4 bytes.
// layers_data: array of pointers to RGBA pixel data, one per layer.
// srgb: true for color textures (default), false for data textures (normal maps).
GpuTexture upload_texture_array(rhi::Rhi& rhi, const u8** layers_data, u32 layer_count,
                                u32 width, u32 height, bool srgb = true);

// Upload 6 RGBA faces as a samplerCube. Face order: +X, -X, +Y, -Y, +Z, -Z.
// Each face must be width*height*4 bytes.
GpuTexture upload_texture_cubemap(rhi::Rhi& rhi, const u8* faces[6],
                                  u32 width, u32 height);

// Stream one RGBA layer into an already-created sampler2DArray at `layer`.
// `pixels` must be exactly width*height*4 bytes and match the array's
// dimensions (no resize here — caller resizes first). Runs the same
// staging + UNDEFINED→TRANSFER_DST→copy→SHADER_READ barrier path as the
// bulk upload_texture_array, scoped to the single target layer. Returns
// false on bad args / staging failure. Used by the GLES unit-texture
// array, which fills layers one at a time as units are registered.
bool upload_array_layer(rhi::Rhi& rhi, rhi::TextureHandle array_tex, u32 layer,
                        const u8* pixels, u32 width, u32 height);

void destroy_texture(rhi::Rhi& rhi, GpuTexture& tex);

} // namespace uldum::render
