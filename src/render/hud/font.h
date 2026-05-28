#pragma once

// Internal header — not part of hud.h's public API. Included by hud.cpp to
// load the default UI font, rasterize glyphs on demand into an alpha-only
// atlas, and expose a descriptor set the text pipeline can bind.
//
// One Font instance wraps:
//   - a FreeType library + FT_Face for the primary font and each fallback
//   - a single 1024×1024 R8_UNORM atlas (GPU image + descriptor set)
//   - a shelf-packer for variable-sized glyph cells
//   - a codepoint → glyph-info cache
//
// Glyphs are rasterized once at `m_em_pixels` via FreeType's normal renderer
// (8-bit grayscale alpha, full hinting). The shader samples the R channel as
// coverage. Quality is sharpest when on-screen size ≈ em_pixels; scaling
// down stays crisp via bilinear; scaling up softens. 32 is a reasonable
// middle for UI sizes 14–48.

#include "core/types.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rhi/handles.h"

namespace uldum::rhi { class Rhi; }

namespace uldum::hud {

class Font {
public:
    // Cached glyph metrics + atlas UV. All distance units are em-normalized
    // (divided by `m_em_pixels`) so the consumer can scale by the desired
    // on-screen pixel size: `on_screen_px = em_units * px_size`. uv is
    // [0, 1] in the atlas texture.
    struct Glyph {
        f32 uv0[2]      = {0, 0};   // atlas UV top-left
        f32 uv1[2]      = {0, 0};   // atlas UV bottom-right
        f32 bearing_x   = 0;        // em: pen-x to glyph-left
        f32 bearing_y   = 0;        // em: baseline to glyph-top
        f32 plane_w     = 0;        // em: glyph quad width
        f32 plane_h     = 0;        // em: glyph quad height
        f32 advance     = 0;        // em: pen advance after this glyph
        bool rasterized = false;    // true once an alpha bitmap is in the atlas
    };

    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Initialize from a platform-curated chain of system font paths.
    // Walks a hardcoded list of well-known OS font locations (Windows:
    // Segoe UI + msyh + emoji; macOS: Helvetica + PingFang + Apple Color
    // Emoji; Android: Roboto + NotoSansCJK + NotoColorEmoji; Linux:
    // DejaVu / Noto family in standard paths). First successful load
    // becomes the primary; subsequent successes are fallbacks consulted
    // per-codepoint.
    //
    // `desc_layout` + `sampler` are HUD-owned and used to bind the atlas
    // into the text pipeline's set 0 / binding 0. Returns false only if
    // no system font loaded at all — Windows / macOS / Android always
    // succeed; minimal Linux installs may fail and need a game-supplied
    // fallback via load_fallback / load_fallback_os_path.
    bool init_from_system(rhi::Rhi& rhi,
                          rhi::DescriptorSetLayoutHandle desc_layout,
                          rhi::SamplerHandle sampler);

    // Update the CJK locale hint used when opening TrueType Collections
    // (NotoSansCJK packs Japanese / Korean / Simplified Chinese /
    // Traditional Chinese as separate faces sharing the same Unicode
    // codepoints — Han unification means 门 / 直 / 説 / 真 have
    // different glyph forms per locale). If the marker (JP/KR/SC/TC)
    // changes, reopens TTC fallback faces at the right index and clears
    // the glyph atlas so already-cached glyphs get re-rasterized.
    // Empty = no preference (face 0 from each .ttc).
    void set_locale(std::string_view bcp47);

    // Add an extra fallback face after `init_from_system`. Optional
    // entry point for game-supplied fonts (consistency across platforms,
    // brand identity). Missing file = no-op.
    //
    // `load_fallback` reads via AssetManager (engine.uldpak / mounted
    // map). `load_fallback_os_path` reads directly from the host
    // filesystem.
    bool load_fallback(std::string_view ttf_path);
    bool load_fallback_os_path(std::string_view filesystem_path);

    void shutdown();

    // Returns metrics for `codepoint`, rasterizing the glyph into the atlas
    // on first request. Returns nullptr for unmappable codepoints or when
    // the atlas is full. Pointer stability: glyphs live in an
    // unordered_map, so pointers returned from earlier calls may be
    // invalidated by later rehashes — callers should copy the struct or
    // re-lookup each frame rather than hold pointers across frames.
    const Glyph* get_glyph(u32 codepoint);

    // Font-wide metrics, in em units.
    f32 line_height() const { return m_line_height; }
    f32 ascent()      const { return m_ascent; }
    f32 descent()     const { return m_descent; }

    // Authored pixel size — what callers use to compute the per-glyph
    // scale factor: on_screen_px = em_units * (target_px / em_pixels()).
    u32 em_pixels() const { return m_em_pixels; }

    rhi::DescriptorSetHandle atlas_descriptor() const { return m_atlas_set; }

    bool valid() const { return m_font != nullptr; }

private:
    bool create_atlas(rhi::DescriptorSetLayoutHandle desc_layout, rhi::SamplerHandle sampler);
    bool rasterize_glyph(u32 codepoint, Glyph& out);
    bool rasterize_glyph_from(void* face_handle, u32 codepoint, Glyph& out);
    bool upload_to_atlas(const u8* alpha, u32 w, u32 h, u32 dst_x, u32 dst_y);
    bool load_fallback_from_bytes(std::string bytes, std::string_view origin);
    bool init_primary_from_bytes(rhi::Rhi& rhi,
                                  std::string bytes,
                                  std::string_view origin,
                                  rhi::DescriptorSetLayoutHandle desc_layout,
                                  rhi::SamplerHandle sampler);

    rhi::Rhi* m_rhi = nullptr;

    // FreeType handles — declared as void* so consumers of font.h don't
    // pull in <ft2build.h>. Cast back in font.cpp.
    void* m_ft   = nullptr;   // FT_Library
    void* m_font = nullptr;   // FT_Face — primary face
    std::string m_ttf_bytes;  // primary font data kept alive for FT_Face lifetime

    // Fallback chain — consulted per-codepoint when the primary face has
    // no glyph for it. Each entry owns its FT_Face + the underlying byte
    // buffer (FT_New_Memory_Face holds a pointer into it).
    struct Fallback {
        void*       handle = nullptr;   // FT_Face
        std::string ttf_bytes;
    };
    std::vector<Fallback> m_fallbacks;

    // Authored rasterization size — every face is configured to this
    // pixel-per-em via FT_Set_Pixel_Sizes before rendering. Consumers
    // multiply em-normalized metrics by the requested on-screen px_size
    // to get the final quad dimensions.
    u32 m_em_pixels = 32;

    // BCP47 hint used by load_fallback_*_path to pick the right face from
    // CJK TrueType Collections. Empty = no preference (face 0).
    std::string m_cjk_lang_hint;

    // Font metrics (em-normalized).
    f32 m_ascent       = 0.0f;
    f32 m_descent      = 0.0f;
    f32 m_line_height  = 0.0f;

    // ── Atlas ────────────────────────────────────────────────────────────
    static constexpr u32 kAtlasSize = 1024;

    rhi::TextureHandle       m_atlas{};
    rhi::DescriptorSetHandle m_atlas_set{};

    // Shelf packer state.
    struct Shelf { u32 y; u32 height; u32 next_x; };
    std::vector<Shelf> m_shelves;
    bool m_atlas_full = false;

    std::unordered_map<u32, Glyph> m_glyphs;
};

} // namespace uldum::hud
