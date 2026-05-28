#include "render/hud/font.h"

#include "rhi/rhi.h"
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
bool Font::init_primary_from_bytes(rhi::Rhi& rhi,
                                     std::string bytes,
                                     std::string_view origin,
                                     rhi::DescriptorSetLayoutHandle desc_layout,
                                     rhi::SamplerHandle sampler) {
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

bool Font::init_from_system(rhi::Rhi& rhi,
                              rhi::DescriptorSetLayoutHandle desc_layout,
                              rhi::SamplerHandle sampler) {
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
        m_rhi->wait_idle();
        if (m_atlas_set.is_valid()) { m_rhi->free_descriptor_set(m_atlas_set); m_atlas_set = {}; }
        m_rhi->destroy_texture(m_atlas);
        m_atlas = {};
    }

    m_glyphs.clear();
    m_shelves.clear();
    m_atlas_full = false;
    m_rhi = nullptr;
}

// ── Atlas creation ────────────────────────────────────────────────────────

bool Font::create_atlas(rhi::DescriptorSetLayoutHandle desc_layout, rhi::SamplerHandle sampler) {
    // Atlas image: R8G8B8A8 — R/G/B = MSDF channels, A = unused (could hold
    // coverage for MTSDF later). Starts zeroed (UNDEFINED) and stays in
    // SHADER_READ_ONLY_OPTIMAL once the first glyph is uploaded; subsequent
    // uploads transition per-copy via begin_oneshot / end_oneshot.
    {
        rhi::TextureDesc td{};
        td.width  = kAtlasSize;
        td.height = kAtlasSize;
        td.format = rhi::TextureFormat::R8G8B8A8_UNORM;
        td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        m_atlas = m_rhi->create_texture(td);
        if (!m_atlas.is_valid()) return false;
    }

    // Clear the atlas to zero so unused regions don't contain garbage
    // memory — bilinear sampling near glyph-cell edges would otherwise
    // pick up random distance-field values and produce visible artifacts.
    // UNDEFINED → TRANSFER_DST → clear → SHADER_READ_ONLY_OPTIMAL.
    rhi::CommandList cmd = m_rhi->begin_oneshot();
    {
        rhi::ImageBarrier b{};
        b.image      = m_atlas;
        b.src_stage  = rhi::PipelineStage::TopOfPipe;
        b.dst_stage  = rhi::PipelineStage::Transfer;
        b.dst_access = rhi::AccessFlag::TransferWrite;
        b.old_layout = rhi::ImageLayout::Undefined;
        b.new_layout = rhi::ImageLayout::TransferDstOptimal;
        cmd.image_barrier(b);

        cmd.clear_color_image(m_atlas, 0.0f, 0.0f, 0.0f, 0.0f);

        rhi::ImageBarrier b2{};
        b2.image      = m_atlas;
        b2.src_stage  = rhi::PipelineStage::Transfer;
        b2.src_access = rhi::AccessFlag::TransferWrite;
        b2.dst_stage  = rhi::PipelineStage::FragmentShader;
        b2.dst_access = rhi::AccessFlag::ShaderRead;
        b2.old_layout = rhi::ImageLayout::TransferDstOptimal;
        b2.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        cmd.image_barrier(b2);
    }
    m_rhi->end_oneshot(cmd);

    m_atlas_set = m_rhi->allocate_descriptor_set(desc_layout);
    if (!m_atlas_set.is_valid()) return false;

    rhi::WriteDescriptor wd{};
    wd.binding = 0;
    wd.type    = rhi::DescriptorType::CombinedImageSampler;
    wd.texture = m_atlas;
    wd.sampler = sampler;
    m_rhi->update_descriptor_set(m_atlas_set, std::span{&wd, 1});
    return true;
}

// ── Glyph rasterization ───────────────────────────────────────────────────

bool Font::upload_to_atlas(const u8* rgba, u32 w, u32 h, u32 dst_x, u32 dst_y) {
    // Staging buffer.
    u64 size = static_cast<u64>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size   = size;
    bd.usage  = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::HostSequential;
    auto stage = m_rhi->create_buffer(bd);
    if (!stage.is_valid()) return false;
    std::memcpy(m_rhi->mapped_ptr(stage), rgba, size);

    rhi::CommandList cmd = m_rhi->begin_oneshot();
    {
        rhi::ImageBarrier b{};
        b.image      = m_atlas;
        b.src_stage  = rhi::PipelineStage::FragmentShader;
        b.src_access = rhi::AccessFlag::ShaderRead;
        b.dst_stage  = rhi::PipelineStage::Transfer;
        b.dst_access = rhi::AccessFlag::TransferWrite;
        b.old_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        b.new_layout = rhi::ImageLayout::TransferDstOptimal;
        cmd.image_barrier(b);

        rhi::BufferImageCopy copy{};
        copy.image_offset_x = static_cast<i32>(dst_x);
        copy.image_offset_y = static_cast<i32>(dst_y);
        copy.image_extent_w = w;
        copy.image_extent_h = h;
        cmd.copy_buffer_to_image(stage, m_atlas, std::span{&copy, 1});

        rhi::ImageBarrier b2{};
        b2.image      = m_atlas;
        b2.src_stage  = rhi::PipelineStage::Transfer;
        b2.src_access = rhi::AccessFlag::TransferWrite;
        b2.dst_stage  = rhi::PipelineStage::FragmentShader;
        b2.dst_access = rhi::AccessFlag::ShaderRead;
        b2.old_layout = rhi::ImageLayout::TransferDstOptimal;
        b2.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        cmd.image_barrier(b2);
    }
    m_rhi->end_oneshot(cmd);
    m_rhi->destroy_buffer(stage);
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
    // Re-orient to non-zero winding rule. CJK fonts (notably NotoSansCJK
    // on Android/Linux) ship glyphs encoded for the even-odd fill rule —
    // inner contours have the same winding as outers — and msdfgen would
    // otherwise treat the glyph body as "outside" and the holes as
    // "inside", producing knockout-text. Idempotent on already-oriented
    // shapes (Microsoft's CJK fonts), so safe to call unconditionally.
    shape.orientContours();
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
