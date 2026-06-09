#include "render/hud/font.h"

#include "rhi/rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

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

// Map a BCP47 locale code to the marker NotoSansCJK uses in family-name
// strings ("Noto Sans CJK SC", "...JP", "...KR", "...TC"). Empty result =
// no CJK preference, pick face 0.
static const char* cjk_marker_for_locale(std::string_view bcp47) {
    auto starts = [&](std::string_view p) {
        return bcp47.size() >= p.size() && bcp47.compare(0, p.size(), p) == 0;
    };
    auto contains = [&](std::string_view needle) {
        return bcp47.find(needle) != std::string_view::npos;
    };
    if (starts("ja"))            return "JP";
    if (starts("ko"))            return "KR";
    if (starts("zh")) {
        // Default zh → Simplified unless an explicit Traditional region.
        if (contains("Hant") || contains("TW") || contains("HK") || contains("MO"))
            return "TC";
        return "SC";
    }
    return "";
}

// Pick the face index in a TTC that best matches `cjk_marker`. Opens each
// face transiently, checks family_name, picks the first that contains the
// marker substring. Returns 0 if nothing matches or the file isn't a TTC.
static FT_Long pick_ttc_face_for_marker(FT_Library lib, const u8* data, FT_Long size,
                                        const char* cjk_marker) {
    if (!cjk_marker || !*cjk_marker) return 0;
    FT_Face probe = nullptr;
    if (FT_New_Memory_Face(lib, data, size, 0, &probe) != 0 || !probe) return 0;
    FT_Long num = probe->num_faces;
    FT_Done_Face(probe);
    if (num <= 1) return 0;
    for (FT_Long i = 0; i < num; ++i) {
        FT_Face f = nullptr;
        if (FT_New_Memory_Face(lib, data, size, i, &f) != 0 || !f) continue;
        const char* fam = f->family_name ? f->family_name : "";
        bool match = std::strstr(fam, cjk_marker) != nullptr;
        FT_Done_Face(f);
        if (match) return i;
    }
    return 0;
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
// Shared between asset-path and OS-path init paths so the FreeType
// plumbing lives in one place.
bool Font::init_primary_from_bytes(rhi::Rhi& rhi,
                                     std::string bytes,
                                     std::string_view origin,
                                     rhi::DescriptorSetLayoutHandle desc_layout,
                                     rhi::SamplerHandle sampler) {
    m_rhi = &rhi;
    m_ttf_bytes = std::move(bytes);

    FT_Library ft = nullptr;
    if (FT_Init_FreeType(&ft) != 0) {
        log::error(TAG, "FT_Init_FreeType failed");
        return false;
    }
    m_ft = ft;

    FT_Face face = nullptr;
    FT_Error err = FT_New_Memory_Face(ft,
                                       reinterpret_cast<const FT_Byte*>(m_ttf_bytes.data()),
                                       static_cast<FT_Long>(m_ttf_bytes.size()),
                                       0, &face);
    if (err != 0 || !face) {
        log::error(TAG, "FT_New_Memory_Face failed for '{}' (err={})", origin, err);
        FT_Done_FreeType(ft);
        m_ft = nullptr;
        m_ttf_bytes.clear();
        return false;
    }
    if (FT_Set_Pixel_Sizes(face, 0, m_em_pixels) != 0) {
        log::warn(TAG, "FT_Set_Pixel_Sizes({}) failed for '{}'", m_em_pixels, origin);
    }
    m_font = face;

    // FreeType reports metrics in 26.6 fixed-point pixels at the active
    // size. Convert to em-normalized: divide by (64 * em_pixels).
    const f32 inv_em_64 = 1.0f / (64.0f * static_cast<f32>(m_em_pixels));
    m_ascent      =  static_cast<f32>(face->size->metrics.ascender)  * inv_em_64;
    m_descent     = -static_cast<f32>(face->size->metrics.descender) * inv_em_64;
    m_line_height =  static_cast<f32>(face->size->metrics.height)    * inv_em_64;

    if (!create_atlas(desc_layout, sampler)) {
        log::error(TAG, "atlas create failed");
        shutdown();
        return false;
    }

    log::info(TAG, "primary font '{}' loaded (em_px={}, ascent={:.2f}, line_h={:.2f})",
              origin, m_em_pixels, m_ascent, m_line_height);
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

    auto* ft = static_cast<FT_Library>(m_ft);
    // If this is a TTC and the active locale wants a specific CJK variant,
    // pick that face instead of face 0 (which on NotoSansCJK is Japanese).
    const FT_Long face_idx = pick_ttc_face_for_marker(
        ft,
        reinterpret_cast<const FT_Byte*>(fb.ttf_bytes.data()),
        static_cast<FT_Long>(fb.ttf_bytes.size()),
        cjk_marker_for_locale(m_cjk_lang_hint));

    FT_Face face = nullptr;
    FT_Error err = FT_New_Memory_Face(ft,
                                       reinterpret_cast<const FT_Byte*>(fb.ttf_bytes.data()),
                                       static_cast<FT_Long>(fb.ttf_bytes.size()),
                                       face_idx, &face);
    if (err != 0 || !face) {
        log::warn(TAG, "FT_New_Memory_Face failed for fallback '{}' (err={})", origin, err);
        return false;
    }
    if (FT_Set_Pixel_Sizes(face, 0, m_em_pixels) != 0) {
        log::warn(TAG, "FT_Set_Pixel_Sizes({}) failed for fallback '{}'", m_em_pixels, origin);
    }
    fb.handle = face;
    const char* fam = face->family_name ? face->family_name : "?";
    m_fallbacks.push_back(std::move(fb));
    log::info(TAG, "loaded fallback font '{}' face {} \"{}\" (#{} in chain)",
              origin, face_idx, fam, m_fallbacks.size());
    return true;
}

void Font::shutdown() {
    for (auto& fb : m_fallbacks) {
        if (fb.handle) FT_Done_Face(static_cast<FT_Face>(fb.handle));
    }
    m_fallbacks.clear();
    if (m_font) { FT_Done_Face(static_cast<FT_Face>(m_font)); m_font = nullptr; }
    if (m_ft)   { FT_Done_FreeType(static_cast<FT_Library>(m_ft)); m_ft = nullptr; }
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
    m_logged_atlas_full = false;
    m_failed_glyphs.clear();
    m_rhi = nullptr;
}

// ── Atlas creation ────────────────────────────────────────────────────────

bool Font::create_atlas(rhi::DescriptorSetLayoutHandle desc_layout, rhi::SamplerHandle sampler) {
    // Atlas image: single-channel R8 holding 8-bit grayscale coverage from
    // FreeType's normal renderer. Starts zeroed (UNDEFINED) and stays in
    // SHADER_READ_ONLY_OPTIMAL once the first glyph is uploaded; subsequent
    // uploads transition per-copy via begin_oneshot / end_oneshot.
    {
        rhi::TextureDesc td{};
        td.width  = kAtlasSize;
        td.height = kAtlasSize;
        td.format = rhi::TextureFormat::R8_UNORM;
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

bool Font::upload_to_atlas(const u8* alpha, u32 w, u32 h, u32 dst_x, u32 dst_y) {
    // Staging buffer — 1 byte per pixel (R8).
    u64 size = static_cast<u64>(w) * h;
    rhi::BufferDesc bd{};
    bd.size   = size;
    bd.usage  = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::HostSequential;
    auto stage = m_rhi->create_buffer(bd);
    if (!stage.is_valid()) return false;
    std::memcpy(m_rhi->mapped_ptr(stage), alpha, size);

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

void Font::set_locale(std::string_view bcp47) {
    if (!m_ft) return;
    const char* old_marker = cjk_marker_for_locale(m_cjk_lang_hint);
    const char* new_marker = cjk_marker_for_locale(bcp47);
    bool marker_changed = std::strcmp(old_marker ? old_marker : "",
                                      new_marker ? new_marker : "") != 0;
    m_cjk_lang_hint.assign(bcp47.data(), bcp47.size());
    if (!marker_changed) return;

    // Reopen each TTC fallback at the face index matching the new marker.
    // Single-face fonts (Roboto on Android, Segoe UI on Windows) yield
    // pick_ttc_face_for_marker == 0, which is what they already had.
    auto* ft = static_cast<FT_Library>(m_ft);
    for (auto& fb : m_fallbacks) {
        if (!fb.handle) continue;
        const FT_Long idx = pick_ttc_face_for_marker(
            ft,
            reinterpret_cast<const FT_Byte*>(fb.ttf_bytes.data()),
            static_cast<FT_Long>(fb.ttf_bytes.size()),
            new_marker);
        FT_Done_Face(static_cast<FT_Face>(fb.handle));
        FT_Face face = nullptr;
        FT_Error err = FT_New_Memory_Face(ft,
                                          reinterpret_cast<const FT_Byte*>(fb.ttf_bytes.data()),
                                          static_cast<FT_Long>(fb.ttf_bytes.size()),
                                          idx, &face);
        if (err != 0 || !face) {
            log::warn(TAG, "set_locale: reopen failed (err={})", err);
            fb.handle = nullptr;
            continue;
        }
        FT_Set_Pixel_Sizes(face, 0, m_em_pixels);
        fb.handle = face;
        log::info(TAG, "set_locale: fallback re-opened at face {} \"{}\"",
                  idx, face->family_name ? face->family_name : "?");
    }

    // Old glyphs were rasterized from the previous face's outlines; their
    // pixels in the atlas are now misleading. Wipe cache + shelves so the
    // next frame re-rasterizes against the new face. The atlas texture
    // itself doesn't strictly need clearing (overwritten lazily as new
    // glyphs land), but reset to zero so stale pixels can't leak through
    // bilinear-edge sampling.
    if (m_rhi) {
        m_rhi->wait_idle();
        rhi::CommandList cmd = m_rhi->begin_oneshot();
        rhi::ImageBarrier b{};
        b.image      = m_atlas;
        b.src_stage  = rhi::PipelineStage::FragmentShader;
        b.src_access = rhi::AccessFlag::ShaderRead;
        b.dst_stage  = rhi::PipelineStage::Transfer;
        b.dst_access = rhi::AccessFlag::TransferWrite;
        b.old_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
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
        m_rhi->end_oneshot(cmd);
    }
    m_glyphs.clear();
    m_shelves.clear();
    m_atlas_full = false;
    m_logged_atlas_full = false;
    m_failed_glyphs.clear();
}

bool Font::rasterize_glyph(u32 codepoint, Glyph& out) {
    // Atlas is full — no point running FreeType load+render across the
    // whole fallback chain just to fail at the packing step. Bail before
    // any FT work. (get_glyph's negative cache also prevents re-entry,
    // but this guards direct callers and keeps the early-out local.)
    if (m_atlas_full) return false;

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

bool Font::rasterize_glyph_from(void* face_handle, u32 codepoint, Glyph& out) {
    if (!face_handle) return false;
    auto face = static_cast<FT_Face>(face_handle);

    // Reject codepoints the face doesn't cover so the fallback chain can
    // walk past. FT_Get_Char_Index returns 0 for missing glyphs (which is
    // also the index of .notdef — we don't want to render the tofu box
    // from the primary face when a fallback might have the real glyph).
    FT_UInt gidx = FT_Get_Char_Index(face, codepoint);
    if (gidx == 0) return false;

    if (FT_Load_Glyph(face, gidx, FT_LOAD_DEFAULT) != 0) return false;
    FT_GlyphSlot slot = face->glyph;

    // advance is in 26.6 fixed-point pixels at the current pixel size;
    // divide by 64 to get pixels, then by em_pixels for em-normalized.
    const f32 inv_em_64 = 1.0f / (64.0f * static_cast<f32>(m_em_pixels));
    out.advance = static_cast<f32>(slot->advance.x) * inv_em_64;

    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return false;
    const FT_Bitmap& bm = slot->bitmap;
    const u32 bw = static_cast<u32>(bm.width);
    const u32 bh = static_cast<u32>(bm.rows);
    if (bw == 0 || bh == 0) {
        // Whitespace / combining mark — has an advance but no pixels.
        // Caller emits no quad for this codepoint.
        out.rasterized = true;
        return true;
    }

    // Shelf-pack into atlas. Choose an existing shelf whose height accepts
    // this glyph; else open a new one. kCellGap pixels of empty space
    // around each cell prevent bilinear sampling at cell-UV edges from
    // bleeding into a neighbor.
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
            // is deferred (LRU or rebuild). Warn ONCE: get_glyph's
            // negative cache + rasterize_glyph's early-out stop the
            // per-glyph-per-frame retry, but this guards against a flood
            // from the very first frame that overflows.
            m_atlas_full = true;
            if (!m_logged_atlas_full) {
                log::warn(TAG, "glyph atlas full — further codepoints will not render "
                               "(no eviction; consider a larger atlas or multi-page support)");
                m_logged_atlas_full = true;
            }
            return false;
        }
        m_shelves.push_back({ next_y, bh, bw + kCellGap });
        dst_x = 0; dst_y = next_y;
    }

    // Pack pixels tightly (FT bitmap may have padding rows via `pitch`).
    // FT pitch is positive for top-down (the default for FT_PIXEL_MODE_GRAY).
    std::vector<u8> pixels(static_cast<size_t>(bw) * bh);
    const int pitch = bm.pitch;
    if (pitch == static_cast<int>(bw)) {
        std::memcpy(pixels.data(), bm.buffer, pixels.size());
    } else {
        for (u32 y = 0; y < bh; ++y) {
            std::memcpy(pixels.data() + static_cast<size_t>(y) * bw,
                        bm.buffer + static_cast<ptrdiff_t>(y) * pitch,
                        bw);
        }
    }
    if (!upload_to_atlas(pixels.data(), bw, bh, dst_x, dst_y)) return false;

    // Glyph metrics in em-normalized units. FT bitmap_left / bitmap_top are
    // in integer pixels — bitmap_top is the distance from the baseline up
    // to the top of the bitmap (ascent-positive), matching our convention.
    const f32 inv_em = 1.0f / static_cast<f32>(m_em_pixels);
    out.bearing_x = static_cast<f32>(slot->bitmap_left) * inv_em;
    out.bearing_y = static_cast<f32>(slot->bitmap_top)  * inv_em;
    out.plane_w   = static_cast<f32>(bw) * inv_em;
    out.plane_h   = static_cast<f32>(bh) * inv_em;

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

    // Negative cache: a codepoint that already failed (atlas full /
    // unsupported / FT error) won't succeed on retry within this
    // session — skip the expensive FreeType load+render and the log.
    if (m_failed_glyphs.count(codepoint)) return nullptr;

    Glyph g{};
    if (!rasterize_glyph(codepoint, g)) {
        m_failed_glyphs.insert(codepoint);
        return nullptr;
    }
    auto [ins, _] = m_glyphs.emplace(codepoint, g);
    return &ins->second;
}

} // namespace uldum::hud
