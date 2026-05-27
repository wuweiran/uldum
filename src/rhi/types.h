#pragma once

// Backend-agnostic resource descriptors. All public RHI surface speaks
// these types — Vk* / Vma* names live only inside src/rhi/vulkan/.
//
// Enums are scoped (`enum class`) for type-safety; flag enums get
// hand-written `|` and `any()` helpers so the call sites stay readable
// without macros.

#include "core/types.h"
#include "rhi/handles.h"

#include <span>

namespace uldum::rhi {

// ─── Buffers ────────────────────────────────────────────────────────────

enum class BufferUsage : u32 {
    None        = 0,
    Vertex      = 1u << 0,
    Index       = 1u << 1,
    Uniform     = 1u << 2,
    Storage     = 1u << 3,
    Indirect    = 1u << 4,
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6,
};

constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool any(BufferUsage value, BufferUsage flag) {
    return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
}

// MemoryUsage governs where the buffer lives + how the CPU may touch it.
// HostSequential / HostRandom buffers are persistently mapped: the RHI
// hands back a stable pointer (see `mapped_ptr`) and the caller writes
// directly — no map/unmap dance.
enum class MemoryUsage {
    GpuOnly,         // device-local; CPU writes go through a staging buffer
    HostSequential,  // host-visible, persistent map, write-only (UBOs, instance SSBOs)
    HostRandom,      // host-visible, persistent map, read+write (rare)
};

struct BufferDesc {
    u64         size   = 0;
    BufferUsage usage  = BufferUsage::None;
    MemoryUsage memory = MemoryUsage::GpuOnly;
};

// ─── Textures ───────────────────────────────────────────────────────────

// Only formats actually used by the engine today are listed. Extend as
// new asset paths land. The enum is u16 to keep TextureDesc tight.
enum class TextureFormat : u16 {
    Undefined = 0,

    // Uncompressed
    R8_UNORM,
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    B8G8R8A8_UNORM,
    B8G8R8A8_SRGB,
    R16_UNORM,
    R16G16B16A16_SFLOAT,
    R32_SFLOAT,
    D32_SFLOAT,
    // Multi-component float and uint formats — primarily used as vertex
    // attributes, not image formats. The enum is named TextureFormat for
    // historical reasons (it'll be split / renamed in Stage 5 if needed).
    R32G32_SFLOAT,
    R32G32B32_SFLOAT,
    R32G32B32A32_SFLOAT,
    R32_UINT,
    R32G32B32A32_UINT,

    // Block-compressed (desktop)
    BC1_RGB_UNORM,
    BC1_RGB_SRGB,
    BC3_RGBA_UNORM,
    BC3_RGBA_SRGB,
    BC5_RG_UNORM,

    // ASTC / ETC2 (mobile, ES backend)
    ASTC_4x4_UNORM,
    ASTC_4x4_SRGB,
    ETC2_RGB8_UNORM,
    ETC2_RGBA8_UNORM,
};

enum class TextureUsage : u32 {
    None            = 0,
    Sampled         = 1u << 0,
    ColorAttachment = 1u << 1,
    DepthAttachment = 1u << 2,
    Storage         = 1u << 3,
    TransferSrc     = 1u << 4,
    TransferDst     = 1u << 5,
};

constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool any(TextureUsage value, TextureUsage flag) {
    return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
}

enum class TextureType : u8 {
    Texture2D,
    TextureCube,
    Texture3D,
};

struct TextureDesc {
    u32           width        = 0;
    u32           height       = 0;
    u32           depth        = 1;  // for Texture3D
    u32           mip_levels   = 1;
    u32           array_layers = 1;  // cubes use 6
    u32           sample_count = 1;  // >1 for MSAA render targets
    TextureFormat format       = TextureFormat::Undefined;
    TextureUsage  usage        = TextureUsage::None;
    TextureType   type         = TextureType::Texture2D;
};

// ─── Samplers ───────────────────────────────────────────────────────────

enum class Filter      : u8 { Nearest, Linear };
enum class MipmapMode  : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
enum class CompareOp   : u8 {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always
};
// Border color used when ClampToBorder addressing samples outside [0,1].
// OpaqueWhite is the canonical "depth=1, sample returns lit" choice for
// shadow PCF samplers; the others cover decals / mask edges.
enum class BorderColor : u8 {
    TransparentBlack,  // (0,0,0,0)
    OpaqueBlack,       // (0,0,0,1)
    OpaqueWhite,       // (1,1,1,1)
};

struct SamplerDesc {
    Filter      mag_filter      = Filter::Linear;
    Filter      min_filter      = Filter::Linear;
    MipmapMode  mipmap_mode     = MipmapMode::Linear;
    AddressMode address_u       = AddressMode::Repeat;
    AddressMode address_v       = AddressMode::Repeat;
    AddressMode address_w       = AddressMode::Repeat;
    f32         mip_lod_bias    = 0.0f;
    f32         max_anisotropy  = 1.0f;
    f32         min_lod         = 0.0f;
    f32         max_lod         = 1000.0f;  // effectively VK_LOD_CLAMP_NONE
    // Shadow / PCF samplers compare the texel against `compare_op` and
    // return 0.0/1.0 instead of the raw value.
    bool        compare_enable  = false;
    CompareOp   compare_op      = CompareOp::Less;
    // Only consulted when an AddressMode is ClampToBorder. Matters for
    // shadow PCF samplers (OpaqueWhite = "lit outside the frustum").
    BorderColor border_color    = BorderColor::TransparentBlack;
};

// ─── Descriptors ────────────────────────────────────────────────────────

enum class DescriptorType : u8 {
    UniformBuffer,
    StorageBuffer,
    SampledImage,           // texture with separate sampler (rare here)
    CombinedImageSampler,   // texture + sampler in one binding (the common case)
};

enum class ShaderStage : u8 {
    None     = 0,
    Vertex   = 1u << 0,
    Fragment = 1u << 1,
    All      = Vertex | Fragment,
};
constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool any(ShaderStage v, ShaderStage f) {
    return (static_cast<u32>(v) & static_cast<u32>(f)) != 0;
}

struct DescriptorSetLayoutBinding {
    u32            binding   = 0;
    DescriptorType type      = DescriptorType::UniformBuffer;
    u32            count     = 1;
    ShaderStage    stages    = ShaderStage::None;
    // Bindless flags — all three usually go together for descriptor-indexing arrays.
    bool partially_bound       = false;
    bool variable_count        = false;
    bool update_after_bind     = false;
};

struct DescriptorSetLayoutDesc {
    std::span<const DescriptorSetLayoutBinding> bindings;
};

struct PushConstantRange {
    ShaderStage stages = ShaderStage::None;
    u32         offset = 0;
    u32         size   = 0;
};

struct PipelineLayoutDesc {
    std::span<const DescriptorSetLayoutHandle> set_layouts;
    std::span<const PushConstantRange>         push_constants;
};

// Tagged union for `update_descriptor_set` writes. Pick the right field
// based on `type` — buffer for *Buffer types, image+sampler for the
// CombinedImageSampler case, etc.
// Per-write image-layout hint. Lives in WriteDescriptor::image_layout
// so binding a depth attachment (DepthStencilReadOnly) as a sampled
// image doesn't need a separate descriptor write path. Default
// (ShaderReadOnly) matches the common case.
enum class WriteImageLayout : u8 {
    ShaderReadOnly,           // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    DepthStencilReadOnly,     // VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    DepthReadOnly,            // VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
    General,                  // VK_IMAGE_LAYOUT_GENERAL
};

struct WriteDescriptor {
    u32            binding       = 0;
    u32            array_element = 0;  // for descriptor arrays (bindless)
    DescriptorType type          = DescriptorType::UniformBuffer;
    // Buffer bindings
    BufferHandle   buffer{};
    u64            buffer_offset = 0;
    u64            buffer_range  = ~0ull;   // whole buffer
    // Image bindings
    TextureHandle    texture{};
    SamplerHandle    sampler{};
    WriteImageLayout image_layout = WriteImageLayout::ShaderReadOnly;
};

// ─── Pipelines ──────────────────────────────────────────────────────────

enum class PrimitiveTopology : u8 { TriangleList, TriangleStrip, LineList };
enum class CullMode         : u8 { None, Front, Back };
enum class FrontFace        : u8 { CounterClockwise, Clockwise };
enum class PolygonMode      : u8 { Fill, Line };
enum class BlendFactor      : u8 {
    Zero, One,
    SrcColor, OneMinusSrcColor,
    DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha,
    DstAlpha, OneMinusDstAlpha,
};
enum class BlendOp          : u8 { Add, Subtract, ReverseSubtract, Min, Max };

struct ShaderStageDesc {
    ShaderStage        stage     = ShaderStage::None;
    ShaderModuleHandle module{};
    const char*        entry     = "main";
};

struct VertexBindingDesc {
    u32 binding   = 0;
    u32 stride    = 0;
    bool per_instance = false;  // false = per-vertex
};

struct VertexAttributeDesc {
    u32           location = 0;
    u32           binding  = 0;
    u32           offset   = 0;
    TextureFormat format   = TextureFormat::Undefined;  // reused for vertex formats
};

struct VertexInputDesc {
    std::span<const VertexBindingDesc>   bindings;
    std::span<const VertexAttributeDesc> attributes;
};

struct RasterizerState {
    PolygonMode polygon_mode      = PolygonMode::Fill;
    CullMode    cull_mode         = CullMode::Back;
    FrontFace   front_face        = FrontFace::CounterClockwise;
    bool        depth_bias_enable = false;
    f32         depth_bias_constant_factor = 0.0f;
    f32         depth_bias_slope_factor    = 0.0f;
    f32         line_width        = 1.0f;
};

struct DepthStencilState {
    bool      depth_test_enable  = true;
    bool      depth_write_enable = true;
    CompareOp depth_compare      = CompareOp::Less;
};

// One per color attachment.
struct BlendAttachmentState {
    bool         blend_enable     = false;
    BlendFactor  src_color_factor = BlendFactor::One;
    BlendFactor  dst_color_factor = BlendFactor::Zero;
    BlendOp      color_op         = BlendOp::Add;
    BlendFactor  src_alpha_factor = BlendFactor::One;
    BlendFactor  dst_alpha_factor = BlendFactor::Zero;
    BlendOp      alpha_op         = BlendOp::Add;
    u8           write_mask       = 0x0F;  // RGBA
};

struct MultisampleState {
    u32 sample_count = 1;
};

// Dynamic-rendering attachment formats (no render pass).
struct RenderAttachmentDesc {
    std::span<const TextureFormat> color_formats;
    TextureFormat                  depth_format = TextureFormat::Undefined;
};

// Backend-agnostic image extent. Structurally compatible with VkExtent2D
// but keeps consumer code free of vulkan.h.
struct Extent2D {
    u32 width  = 0;
    u32 height = 0;
};

struct GraphicsPipelineDesc {
    PipelineLayoutHandle                 layout;
    std::span<const ShaderStageDesc>     stages;
    VertexInputDesc                      vertex_input;
    PrimitiveTopology                    topology = PrimitiveTopology::TriangleList;
    RasterizerState                      rasterizer;
    DepthStencilState                    depth_stencil;
    std::span<const BlendAttachmentState> blend_attachments;
    MultisampleState                     multisample;
    RenderAttachmentDesc                 render;
};

} // namespace uldum::rhi
