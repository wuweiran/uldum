#include "render/hud/font.h"

#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#if defined(_WIN32)
// `GetWindowsDirectoryA` to resolve %WINDIR% reliably without tripping
// MSVC's std::getenv deprecation warning. Defined in <windows.h>.
// WIN32_LEAN_AND_MEAN is already set globally by CMake.
#include <windows.h>
#endif

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

// Read an entire file from the filesystem into a string buffer. Used for
// system-font paths (`C:/Windows/Fonts/…`, `/System/Library/Fonts/…`)
// that AssetManager doesn't have mounted.
static std::string read_os_file(std::string_view filesystem_path) {
    std::ifstream f(std::string(filesystem_path), std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return std::move(ss).str();
}

// Initialize the primary face from a byte buffer + create the atlas.
// Shared between asset-path and OS-path init paths so the FreeType /
// msdfgen plumbing lives in one place.
bool Font::init_primary_from_bytes(rhi::VulkanRhi& rhi,
                                     std::string bytes,
                                     std::string_view origin,
                                     VkDescriptorSetLayout desc_layout,
                                     VkSampler sampler) {
    m_rhi = &rhi;
    m_ttf_bytes = std::move(bytes);

    auto* ft = msdfgen::initializeFreetype();
    if (!ft) { log::error(TAG, "msdfgen FreeType init failed"); return false; }
    m_ft = ft;

    auto* font = msdfgen::loadFontData(ft,
                    reinterpret_cast<const msdfgen::byte*>(m_ttf_bytes.data()),
                    static_cast<int>(m_ttf_bytes.size()));
    if (!font) {
        log::error(TAG, "msdfgen loadFontData failed for '{}'", origin);
        msdfgen::deinitializeFreetype(ft);
        m_ft = nullptr;
        m_ttf_bytes.clear();
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

    log::info(TAG, "primary font '{}' loaded (em={:.2f}, ascent={:.2f}, line_h={:.2f})",
              origin, m_em_size, m_ascent, m_line_height);
    return true;
}

bool Font::init_from_system(rhi::VulkanRhi& rhi,
                              VkDescriptorSetLayout desc_layout,
                              VkSampler sampler) {
    // Per-platform list of well-known font paths. First entry that loads
    // becomes the primary; the rest are added as fallbacks consulted
    // per-codepoint. The chain aims to cover all common scripts using
    // fonts the OS ships by default.
    //
    // Limitation: hardcoded paths can break on unusual installs (a
    // missing optional font, a non-default install location). The
    // "proper" answer is OS font-matching APIs (DirectWrite,
    // CoreText, fontconfig); those are a real integration project
    // and deferred.
    std::vector<std::string> paths;
#if defined(_WIN32)
    // Windows fonts live under %WINDIR%\Fonts. WINDIR is set on every
    // Windows install — typically `C:\Windows` but can be on any drive
    // (`D:\Windows`, `E:\WINNT`, etc.). GetWindowsDirectoryA is the
    // canonical Win32 lookup; falls back to a sensible default if the
    // call ever fails (which it doesn't on a normal Windows process).
    std::string fonts_dir;
    {
        char buf[MAX_PATH];
        UINT n = GetWindowsDirectoryA(buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            fonts_dir = std::string(buf, n) + "/Fonts/";
        } else {
            fonts_dir = "C:/Windows/Fonts/";
        }
    }
    paths = {
        // Primary: Segoe UI covers Latin / Cyrillic / Greek / Arabic /
        // Hebrew / Vietnamese on Windows 7+.
        fonts_dir + "segoeui.ttf",
        // CJK — Simplified, Traditional, Japanese, Korean. Multiple
        // entries because no single Windows font covers everything;
        // first-with-glyph wins per codepoint.
        fonts_dir + "msyh.ttc",        // Microsoft YaHei (CJK Simplified)
        fonts_dir + "simsun.ttc",      // SimSun (CJK Simplified, legacy)
        fonts_dir + "msjh.ttc",        // Microsoft JhengHei (CJK Traditional)
        fonts_dir + "YuGothR.ttc",     // Yu Gothic (Japanese)
        fonts_dir + "malgun.ttf",      // Malgun Gothic (Korean)
        // Indic + Southeast Asian scripts.
        fonts_dir + "Nirmala.ttf",     // Devanagari, Tamil, Telugu, Bengali, ...
        fonts_dir + "Leelawui.ttf",    // Thai, Lao
        // Emoji (color).
        fonts_dir + "seguiemj.ttf",
    };
#elif defined(__APPLE__)
    paths = {
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/PingFang.ttc",        // CJK Simplified/Traditional
        "/System/Library/Fonts/Hiragino Sans GB.ttc",// CJK Simplified
        "/System/Library/Fonts/HiraginoSans.ttc",    // Japanese (varies by macOS)
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",// Korean
        "/System/Library/Fonts/Devanagari MT.ttc",   // Devanagari
        "/System/Library/Fonts/ThonburiUI.ttc",      // Thai
        "/System/Library/Fonts/Apple Color Emoji.ttc",
    };
#elif defined(__ANDROID__)
    paths = {
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansDevanagari-Regular.ttf",
        "/system/fonts/NotoSansThai-Regular.ttf",
        "/system/fonts/NotoColorEmoji.ttf",
    };
#elif defined(__linux__)
    // Linux: distros vary too much for a tight list. Try the common
    // Debian/Ubuntu/Fedora/Arch paths for DejaVu + Noto.
    paths = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
    };
#else
    // Unknown platform — no system paths.
#endif

    for (const auto& p : paths) {
        std::string bytes = read_os_file(p);
        if (bytes.empty()) continue;
        if (!m_font) {
            // First successful load → primary.
            if (!init_primary_from_bytes(rhi, std::move(bytes), p,
                                          desc_layout, sampler)) {
                // Couldn't make this one work (parse fail / atlas alloc
                // fail). Skip and try the next path as primary.
                continue;
            }
        } else {
            load_fallback_from_bytes(std::move(bytes), p);
        }
    }

    if (!m_font) {
        log::error(TAG, "init_from_system: no usable system font found; "
                        "text rendering disabled. Provide a game-supplied "
                        "fallback via Font::load_fallback_os_path.");
        return false;
    }
    return true;
}

bool Font::load_fallback(std::string_view ttf_path) {
    if (!m_ft) {
        log::warn(TAG, "load_fallback called before primary font init — ignored");
        return false;
    }
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) { log::warn(TAG, "AssetManager not initialized for fallback"); return false; }
    auto bytes = mgr->read_file_bytes(ttf_path);
    if (bytes.empty()) {
        // Quiet — fallback fonts are optional; absence is not an error.
        return false;
    }
    std::string buf(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return load_fallback_from_bytes(std::move(buf), ttf_path);
}

bool Font::load_fallback_os_path(std::string_view filesystem_path) {
    if (!m_ft) {
        log::warn(TAG, "load_fallback_os_path called before primary font init — ignored");
        return false;
    }
    std::string buf = read_os_file(filesystem_path);
    if (buf.empty()) return false;  // quiet — optional
    return load_fallback_from_bytes(std::move(buf), filesystem_path);
}

bool Font::load_fallback_from_bytes(std::string bytes, std::string_view origin) {
    Fallback fb;
    fb.ttf_bytes = std::move(bytes);

    auto* ft = static_cast<msdfgen::FreetypeHandle*>(m_ft);
    auto* font = msdfgen::loadFontData(ft,
                    reinterpret_cast<const msdfgen::byte*>(fb.ttf_bytes.data()),
                    static_cast<int>(fb.ttf_bytes.size()));
    if (!font) {
        log::warn(TAG, "msdfgen loadFontData failed for fallback '{}'", origin);
        return false;
    }
    fb.handle = font;
    m_fallbacks.push_back(std::move(fb));
    log::info(TAG, "loaded fallback font '{}' (fallback #{})", origin, m_fallbacks.size());
    return true;
}

void Font::shutdown() {
    for (auto& fb : m_fallbacks) {
        if (fb.handle) msdfgen::destroyFont(static_cast<msdfgen::FontHandle*>(fb.handle));
    }
    m_fallbacks.clear();
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
    // Walk the chain: primary first, then each registered fallback.
    // First face with a glyph for `codepoint` wins. Whitespace glyphs (no
    // outline) count as "present" — they have an advance and stop the
    // walk; we don't want to keep searching past a primary's space.
    if (m_font && rasterize_glyph_from(m_font, codepoint, out)) return true;
    for (auto& fb : m_fallbacks) {
        if (rasterize_glyph_from(fb.handle, codepoint, out)) return true;
    }
    return false;
}

bool Font::rasterize_glyph_from(void* font_handle, u32 codepoint, Glyph& out) {
    if (!font_handle) return false;

    auto* font = static_cast<msdfgen::FontHandle*>(font_handle);

    // Check the codepoint actually exists in this face BEFORE asking
    // msdfgen for a glyph. `msdfgen::loadGlyph` silently loads .notdef
    // (the tofu box) when a codepoint is missing from the font and
    // returns *true* — which would prevent the fallback chain from ever
    // walking past the primary. `getGlyphIndex` returns false when the
    // FT_Get_Char_Index lookup yields 0, which is the correct signal.
    msdfgen::GlyphIndex glyph_idx;
    if (!msdfgen::getGlyphIndex(glyph_idx, font,
                                  static_cast<msdfgen::unicode_t>(codepoint))) {
        return false;
    }

    double advance = 0.0;
    msdfgen::Shape shape;
    if (!msdfgen::loadGlyph(shape, font, glyph_idx,
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
