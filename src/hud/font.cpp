#include "hud/font.h"

#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace uldum::hud {

static constexpr const char* TAG = "HUD.Font";

// Clamp a float (from MSDF output, nominally in ~[0, 1]) into a u8. MSDF
// values can land slightly outside [0, 1] near edges — clamping is safer
// than relying on the shader to cope.
static inline u8 to_u8(float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return static_cast<u8>(f * 255.0f + 0.5f);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

Font::Font() = default;
Font::~Font() { shutdown(); }

bool Font::init(rhi::VulkanRhi& rhi,
                std::string_view ttf_path,
                VkDescriptorSetLayout desc_layout,
                VkSampler sampler) {
    m_rhi = &rhi;

    // Read the .ttf into memory (msdfgen's loadFontData keeps a reference,
    // so we hold the bytes alive in m_ttf_bytes for the Font's lifetime).
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) { log::error(TAG, "AssetManager not initialized"); return false; }
    auto bytes = mgr->read_file_bytes(ttf_path);
    if (bytes.empty()) {
        log::warn(TAG, "font file missing: '{}'; text rendering disabled", ttf_path);
        return false;
    }
    m_ttf_bytes.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    auto* ft = msdfgen::initializeFreetype();
    if (!ft) { log::error(TAG, "msdfgen FreeType init failed"); return false; }
    m_ft = ft;

    auto* font = msdfgen::loadFontData(ft,
                    reinterpret_cast<const msdfgen::byte*>(m_ttf_bytes.data()),
                    static_cast<int>(m_ttf_bytes.size()));
    if (!font) {
        log::error(TAG, "msdfgen loadFontData failed for '{}'", ttf_path);
        msdfgen::deinitializeFreetype(ft);
        m_ft = nullptr;
        return false;
    }
    m_font = font;

    msdfgen::FontMetrics fm{};
    msdfgen::getFontMetrics(fm, font, msdfgen::FONT_SCALING_EM_NORMALIZED);
    m_em_size     = static_cast<f32>(fm.emSize);
    m_ascent      = static_cast<f32>(fm.ascenderY);
    m_descent     = static_cast<f32>(fm.descenderY);
    m_line_height = static_cast<f32>(fm.lineHeight);

    if (!create_atlas(desc_layout, sampler)) {
        log::error(TAG, "atlas create failed");
        shutdown();
        return false;
    }

    log::info(TAG, "font '{}' loaded (em={:.2f}, ascent={:.2f}, line_h={:.2f})",
              ttf_path, m_em_size, m_ascent, m_line_height);
    return true;
}

void Font::shutdown() {
    if (m_font) { msdfgen::destroyFont(static_cast<msdfgen::FontHandle*>(m_font)); m_font = nullptr; }
    if (m_ft)   { msdfgen::deinitializeFreetype(static_cast<msdfgen::FreetypeHandle*>(m_ft)); m_ft = nullptr; }
    m_ttf_bytes.clear();

    if (m_rhi) {
        VkDevice device = m_rhi->device();
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (m_atlas_set)   { vkFreeDescriptorSets(device, m_atlas_pool, 1, &m_atlas_set); m_atlas_set = VK_NULL_HANDLE; }
            if (m_atlas_pool)  { vkDestroyDescriptorPool(device, m_atlas_pool, nullptr);      m_atlas_pool = VK_NULL_HANDLE; }
            if (m_atlas_view)  { vkDestroyImageView(device, m_atlas_view, nullptr);           m_atlas_view = VK_NULL_HANDLE; }
            if (m_atlas_image) { vmaDestroyImage(m_rhi->allocator(), m_atlas_image, m_atlas_alloc); m_atlas_image = VK_NULL_HANDLE; m_atlas_alloc = VK_NULL_HANDLE; }
        }
    }

    m_glyphs.clear();
    m_shelves.clear();
    m_atlas_full = false;
    m_rhi = nullptr;
}

// ── Atlas creation ────────────────────────────────────────────────────────

bool Font::create_atlas(VkDescriptorSetLayout desc_layout, VkSampler sampler) {
    VkDevice device = m_rhi->device();

    // Atlas image: R8G8B8A8 — R/G/B = MSDF channels, A = unused (could hold
    // coverage for MTSDF later). Starts zeroed (UNDEFINED) and stays in
    // SHADER_READ_ONLY_OPTIMAL once the first glyph is uploaded; subsequent
    // uploads transition per-copy via begin_oneshot / end_oneshot.
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { kAtlasSize, kAtlasSize, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_rhi->allocator(), &ici, &aci, &m_atlas_image, &m_atlas_alloc, nullptr) != VK_SUCCESS) {
        return false;
    }

    // Clear the atlas to zero so unused regions don't contain garbage
    // memory — bilinear sampling near glyph-cell edges would otherwise
    // pick up random distance-field values and produce visible artifacts.
    // UNDEFINED → TRANSFER_DST → clear → SHADER_READ_ONLY_OPTIMAL.
    VkCommandBuffer cmd = m_rhi->begin_oneshot();
    {
        VkImageMemoryBarrier to_xfer{};
        to_xfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_xfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_xfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_xfer.srcAccessMask       = 0;
        to_xfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_xfer.image               = m_atlas_image;
        to_xfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_xfer);

        VkClearColorValue clear{};  // zero-init: R=G=B=A=0
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);

        VkImageMemoryBarrier to_read{};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        to_read.image               = m_atlas_image;
        to_read.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
    }
    m_rhi->end_oneshot(cmd);

    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = m_atlas_image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vci, nullptr, &m_atlas_view) != VK_SUCCESS) return false;

    // Descriptor pool for this atlas set. Sized 1 — we never allocate more.
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets       = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &sz;
    if (vkCreateDescriptorPool(device, &pci, nullptr, &m_atlas_pool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo dai{};
    dai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool     = m_atlas_pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &desc_layout;
    if (vkAllocateDescriptorSets(device, &dai, &m_atlas_set) != VK_SUCCESS) return false;

    VkDescriptorImageInfo img{};
    img.sampler     = sampler;
    img.imageView   = m_atlas_view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = m_atlas_set;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &img;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    return true;
}

// ── Glyph rasterization ───────────────────────────────────────────────────

bool Font::upload_to_atlas(const u8* rgba, u32 w, u32 h, u32 dst_x, u32 dst_y) {
    // Staging buffer.
    VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer stage = VK_NULL_HANDLE;
    VmaAllocation stage_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo stage_info{};
    if (vmaCreateBuffer(m_rhi->allocator(), &bci, &aci, &stage, &stage_alloc, &stage_info) != VK_SUCCESS) return false;
    std::memcpy(stage_info.pMappedData, rgba, size);

    VkCommandBuffer cmd = m_rhi->begin_oneshot();
    {
        VkImageMemoryBarrier to_xfer{};
        to_xfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_xfer.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_xfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_xfer.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        to_xfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_xfer.image               = m_atlas_image;
        to_xfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_xfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset      = { static_cast<i32>(dst_x), static_cast<i32>(dst_y), 0 };
        copy.imageExtent      = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, stage, m_atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier to_read{};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        to_read.image               = m_atlas_image;
        to_read.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
    }
    m_rhi->end_oneshot(cmd);
    vmaDestroyBuffer(m_rhi->allocator(), stage, stage_alloc);
    return true;
}

bool Font::rasterize_glyph(u32 codepoint, Glyph& out) {
    if (!m_font) return false;

    auto* font = static_cast<msdfgen::FontHandle*>(m_font);
    double advance = 0.0;
    msdfgen::Shape shape;
    if (!msdfgen::loadGlyph(shape, font, static_cast<msdfgen::unicode_t>(codepoint),
                            msdfgen::FONT_SCALING_EM_NORMALIZED, &advance)) {
        return false;
    }
    out.advance = static_cast<f32>(advance);

    if (!shape.validate() || shape.contours.empty()) {
        // Whitespace / combining mark — no quad to rasterize. Legal and
        // common (space U+0020 has an advance but no outline). Caller just
        // emits no quad for this codepoint.
        out.rasterized = true;
        return true;
    }

    shape.normalize();
    msdfgen::edgeColoringSimple(shape, 3.0);

    // Bounds in em units → pixel footprint. Scale such that 1 em maps to
    // m_em_pixels, then add m_msdf_padding on each side for the distance
    // field to fade into.
    auto b = shape.getBounds();
    f32 em_w = static_cast<f32>(b.r - b.l);
    f32 em_h = static_cast<f32>(b.t - b.b);
    if (em_w <= 0.0f || em_h <= 0.0f) { out.rasterized = true; return true; }

    u32 bw = static_cast<u32>(std::ceil(em_w * m_em_pixels)) + 2u * m_msdf_padding;
    u32 bh = static_cast<u32>(std::ceil(em_h * m_em_pixels)) + 2u * m_msdf_padding;

    // Shelf-pack into atlas. Choose an existing shelf whose height accepts
    // this glyph; else open a new one. We leave kCellGap pixels of empty
    // space around each cell so bilinear sampling at cell-UV edges fetches
    // from cleared atlas pixels (coverage = 0) instead of the neighbor's
    // distance field.
    constexpr u32 kCellGap = 1;
    u32 dst_x = 0, dst_y = 0;
    bool placed = false;
    for (auto& shelf : m_shelves) {
        if (shelf.height >= bh && shelf.next_x + bw <= kAtlasSize) {
            dst_x = shelf.next_x; dst_y = shelf.y;
            shelf.next_x += bw + kCellGap;
            placed = true;
            break;
        }
    }
    if (!placed) {
        u32 next_y = m_shelves.empty() ? 0u
                                       : (m_shelves.back().y + m_shelves.back().height + kCellGap);
        if (next_y + bh > kAtlasSize) {
            // Atlas full. Drop further rasterization requests — eviction
            // is deferred to Stage C+ (LRU or rebuild).
            m_atlas_full = true;
            log::warn(TAG, "glyph atlas full — codepoint {} skipped", codepoint);
            return false;
        }
        m_shelves.push_back({ next_y, bh, bw + kCellGap });
        dst_x = 0; dst_y = next_y;
        placed = true;
    }

    // Generate MSDF. Projection maps em-space → bitmap-space:
    //   - scale = m_em_pixels (em → pixels)
    //   - translate shifts (bounds.l, bounds.b) to (padding, padding)
    msdfgen::Vector2 scale(m_em_pixels, m_em_pixels);
    msdfgen::Vector2 translate(-b.l + m_msdf_padding / scale.x,
                               -b.b + m_msdf_padding / scale.y);
    // Range is expressed in em units here (coordinate of the Projection);
    // 2 pixels each side is a reasonable default for crisp UI text.
    msdfgen::Range range(4.0 / m_em_pixels);

    msdfgen::Bitmap<float, 3> msdf(static_cast<int>(bw), static_cast<int>(bh));
    msdfgen::generateMSDF(msdf, shape,
                          msdfgen::SDFTransformation(msdfgen::Projection(scale, translate), range));

    // Convert float RGB → u8 RGBA (A always 255 — unused channel for plain MSDF).
    std::vector<u8> rgba(static_cast<size_t>(bw) * bh * 4);
    for (u32 y = 0; y < bh; ++y) {
        for (u32 x = 0; x < bw; ++x) {
            // msdfgen bitmap rows are bottom-to-top. Flip vertically so
            // the atlas upload matches our screen-coord UV convention.
            const float* px = msdf(static_cast<int>(x), static_cast<int>(bh - 1 - y));
            size_t idx = (static_cast<size_t>(y) * bw + x) * 4;
            rgba[idx + 0] = to_u8(px[0]);
            rgba[idx + 1] = to_u8(px[1]);
            rgba[idx + 2] = to_u8(px[2]);
            rgba[idx + 3] = 255;
        }
    }
    if (!upload_to_atlas(rgba.data(), bw, bh, dst_x, dst_y)) return false;

    // Fill glyph metrics. plane_* is the quad in em units; bearing_* places
    // it relative to the pen. Subtract/add the padding (in em units) so
    // the quad matches only the glyph, not the distance-field margin.
    f32 pad_em = static_cast<f32>(m_msdf_padding) / m_em_pixels;
    out.bearing_x = static_cast<f32>(b.l) - pad_em;
    out.bearing_y = static_cast<f32>(b.t) + pad_em;  // ascent-positive
    out.plane_w   = em_w + 2.0f * pad_em;
    out.plane_h   = em_h + 2.0f * pad_em;

    out.uv0[0] = static_cast<f32>(dst_x) / kAtlasSize;
    out.uv0[1] = static_cast<f32>(dst_y) / kAtlasSize;
    out.uv1[0] = static_cast<f32>(dst_x + bw) / kAtlasSize;
    out.uv1[1] = static_cast<f32>(dst_y + bh) / kAtlasSize;
    out.rasterized = true;
    return true;
}

const Font::Glyph* Font::get_glyph(u32 codepoint) {
    auto it = m_glyphs.find(codepoint);
    if (it != m_glyphs.end()) return &it->second;

    Glyph g{};
    if (!rasterize_glyph(codepoint, g)) return nullptr;
    auto [ins, _] = m_glyphs.emplace(codepoint, g);
    return &ins->second;
}

} // namespace uldum::hud
