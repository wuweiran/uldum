// OpenGL ES 3.1 RHI backend — implementation.
//
// Active on Android when ULDUM_BACKEND_GLES is enabled. Mirrors the
// Vulkan backend's class shape over EGL + ES 3.1: EGL context/surface,
// buffers, textures, samplers, GLSL ES 3.10 shader modules, descriptor
// binding-table emulation, program-linked pipelines, and the
// eglSwapBuffers frame loop. CommandList bodies live in
// gles/command_list.cpp.

#include "rhi/gles/gles_rhi.h"
#include "rhi/gles/gles_records.h"
#include "rhi/detail/slot_table.h"
#include "platform/platform.h"
#include "core/log.h"

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>  // EXT_buffer_storage (GL_MAP_PERSISTENT_BIT_EXT etc.)
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace uldum::rhi {

static constexpr const char* TAG = "RHI.GLES";

// Record type definitions live in gles_records.h (included above) so
// gles/command_list.cpp can also see them.

// ── Impl — holds EGL context, record tables, free lists ──

struct Rhi::Impl {
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig  egl_config  = nullptr;

    std::vector<BufferRecord>              buffers;
    std::vector<u32>                       buffers_free;
    std::vector<TextureRecord>             textures;
    std::vector<u32>                       textures_free;
    std::vector<SamplerRecord>             samplers;
    std::vector<u32>                       samplers_free;
    std::vector<ShaderModuleRecord>        shader_modules;
    std::vector<u32>                       shader_modules_free;
    std::vector<DescriptorSetLayoutRecord> dsl_records;
    std::vector<u32>                       dsl_free;
    std::vector<DescriptorSetRecord>       dset_records;
    std::vector<u32>                       dset_free;
    std::vector<PipelineLayoutRecord>      pl_records;
    std::vector<u32>                       pl_free;
    std::vector<PipelineRecord>            pipeline_records;
    std::vector<u32>                       pipeline_free;

    // GLES baseInstance emulation. GLES 3.1 ignores the firstInstance
    // field of multi-draw-indirect commands when computing
    // gl_InstanceID. We feed it manually via a small UBO at slot 31
    // that every vertex shader reads and adds to gl_InstanceID. One
    // UBO for the whole RHI — written by draw_indexed_indirect /
    // draw_indexed before each glDrawElements* call.
    GLuint  draw_info_ubo = 0;
    u32     last_base_instance = 0xFFFFFFFFu;  // force first upload
};

// ── Slab-allocator helpers (shared with the Vulkan backend) ─────────

namespace {
// acquire_slot / bump_generation / lookup live in
// rhi/detail/slot_table.h. Pull them into this TU's unqualified scope
// so call sites read unchanged.
using detail::acquire_slot;
using detail::bump_generation;
using detail::lookup;
} // namespace

// ── Format / usage translation ──────────────────────────────────────

namespace {
// (sized_internal_format, format, type)
struct GlFormatTriplet { GLenum internal; GLenum format; GLenum type; };

GlFormatTriplet to_gl_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::R8_UNORM:            return { GL_R8,                GL_RED,             GL_UNSIGNED_BYTE };
        case TextureFormat::R8G8B8A8_UNORM:      return { GL_RGBA8,             GL_RGBA,            GL_UNSIGNED_BYTE };
        case TextureFormat::R8G8B8A8_SRGB:       return { GL_SRGB8_ALPHA8,      GL_RGBA,            GL_UNSIGNED_BYTE };
        case TextureFormat::B8G8R8A8_UNORM:      return { GL_RGBA8,             GL_RGBA,            GL_UNSIGNED_BYTE };  // swizzled in shader if BGRA needed
        case TextureFormat::B8G8R8A8_SRGB:       return { GL_SRGB8_ALPHA8,      GL_RGBA,            GL_UNSIGNED_BYTE };
        case TextureFormat::R16_UNORM:           return { GL_R16_EXT,           GL_RED,             GL_UNSIGNED_SHORT };
        case TextureFormat::R16G16B16A16_SFLOAT: return { GL_RGBA16F,           GL_RGBA,            GL_HALF_FLOAT };
        case TextureFormat::R32_SFLOAT:          return { GL_R32F,              GL_RED,             GL_FLOAT };
        case TextureFormat::R32G32_SFLOAT:       return { GL_RG32F,             GL_RG,              GL_FLOAT };
        case TextureFormat::R32G32B32A32_SFLOAT: return { GL_RGBA32F,           GL_RGBA,            GL_FLOAT };
        case TextureFormat::R32G32B32_SFLOAT:    return { GL_RGB32F,            GL_RGB,             GL_FLOAT };
        case TextureFormat::R32_UINT:            return { GL_R32UI,             GL_RED_INTEGER,     GL_UNSIGNED_INT };
        case TextureFormat::R32G32B32A32_UINT:   return { GL_RGBA32UI,          GL_RGBA_INTEGER,    GL_UNSIGNED_INT };
        case TextureFormat::D32_SFLOAT:          return { GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT };
        case TextureFormat::BC1_RGB_UNORM:
        case TextureFormat::BC1_RGB_SRGB:
        case TextureFormat::BC3_RGBA_UNORM:
        case TextureFormat::BC3_RGBA_SRGB:
        case TextureFormat::BC5_RG_UNORM:
            // GLES uses ETC2/ASTC, not BC. Texture assets need re-encoding
            // for the GLES target (handled by the asset packer when shipping
            // an Android bundle).
            log::warn(TAG, "BC compressed texture not supported on GLES; asset packer must re-encode");
            return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
        case TextureFormat::Undefined:
            break;
    }
    return { 0, 0, 0 };
}

GLenum to_gl_buffer_target(BufferUsage u) {
    // Pick the most specific binding point. GL allows the same buffer to be
    // bound elsewhere later (e.g. a vertex buffer also used as TransferSrc),
    // so this is just the "natural home" — we rebind as needed.
    if (any(u, BufferUsage::Index))       return GL_ELEMENT_ARRAY_BUFFER;
    if (any(u, BufferUsage::Uniform))     return GL_UNIFORM_BUFFER;
    if (any(u, BufferUsage::Storage))     return GL_SHADER_STORAGE_BUFFER;
    if (any(u, BufferUsage::Indirect))    return GL_DRAW_INDIRECT_BUFFER;
    if (any(u, BufferUsage::TransferSrc)) return GL_PIXEL_UNPACK_BUFFER;
    return GL_ARRAY_BUFFER;  // default = vertex
}

GLenum to_gl_buffer_usage_hint(MemoryUsage m) {
    switch (m) {
        case MemoryUsage::GpuOnly:        return GL_STATIC_DRAW;
        case MemoryUsage::HostSequential: return GL_STREAM_DRAW;
        case MemoryUsage::HostRandom:     return GL_DYNAMIC_DRAW;
    }
    return GL_STATIC_DRAW;
}

GLenum to_gl_wrap(AddressMode a) {
    switch (a) {
        case AddressMode::Repeat:         return GL_REPEAT;
        case AddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:    return GL_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:
            // ES 3.1 has no GL_CLAMP_TO_BORDER without EXT_texture_border_clamp.
            // Substitute ClampToEdge — visually wrong for shadow maps (PCF
            // samples bleed at edges) but correct enough to boot. If shadow
            // quality matters later, gate this on EXT_texture_border_clamp.
            return GL_CLAMP_TO_EDGE;
    }
    return GL_CLAMP_TO_EDGE;
}

GLenum to_gl_min_filter(Filter f, MipmapMode m, f32 max_lod) {
    const bool mipped = (max_lod > 0.0f);
    if (!mipped) return (f == Filter::Linear) ? GL_LINEAR : GL_NEAREST;
    if (f == Filter::Linear) {
        return (m == MipmapMode::Linear) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
    }
    return (m == MipmapMode::Linear) ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
}
} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────

Rhi::Rhi() : m_impl(new Impl{}) {}
Rhi::~Rhi() { shutdown(); delete m_impl; m_impl = nullptr; }

// EGL_KHR_gl_colorspace token values — hard-coded so we don't depend on
// the header shipping the extension defines. EGL_KHR_gl_colorspace has
// been an Android core extension since API 17; we just have to ask for
// it. The driver still validates the request and falls back to no
// extension list if it doesn't support sRGB on this config.
#ifndef EGL_GL_COLORSPACE_KHR
#define EGL_GL_COLORSPACE_KHR       0x309D
#endif
#ifndef EGL_GL_COLORSPACE_SRGB_KHR
#define EGL_GL_COLORSPACE_SRGB_KHR  0x3089
#endif
#ifndef EGL_GL_COLORSPACE_LINEAR_KHR
#define EGL_GL_COLORSPACE_LINEAR_KHR 0x308A
#endif

// Create the window surface with an sRGB color space when the driver
// supports it. Falls back silently to a default (linear) surface so an
// older driver gets pre-fix dim output rather than a hard failure.
static EGLSurface create_window_surface_srgb(EGLDisplay display, EGLConfig config,
                                              void* native_window) {
    // Log whether the driver advertises EGL_KHR_gl_colorspace so we
    // know up front whether the sRGB request can succeed. Returning an
    // unsupported attribute usually fails with EGL_BAD_ATTRIBUTE.
    const char* exts = eglQueryString(display, EGL_EXTENSIONS);
    bool has_srgb_ext = exts && std::string_view(exts).find("EGL_KHR_gl_colorspace") != std::string_view::npos;
    log::info(TAG, "EGL extensions advertise sRGB color-space: {}",
              has_srgb_ext ? "yes" : "no");

    const EGLint srgb_attrs[] = {
        EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR,
        EGL_NONE
    };
    EGLSurface surf = eglCreateWindowSurface(display, config,
        static_cast<EGLNativeWindowType>(native_window), srgb_attrs);
    if (surf == EGL_NO_SURFACE) {
        EGLint err = eglGetError();
        log::warn(TAG, "sRGB EGL surface not supported (err 0x{:x}); "
                       "falling back to linear — colors will look dim", err);
        surf = eglCreateWindowSurface(display, config,
            static_cast<EGLNativeWindowType>(native_window), nullptr);
    } else {
        // Verify what the driver actually gave us. Some drivers
        // silently downgrade to linear even though they returned a
        // surface — querying after creation is the only way to know.
        EGLint colorspace = 0;
        if (eglQuerySurface(display, surf, EGL_GL_COLORSPACE_KHR, &colorspace)) {
            log::info(TAG, "EGL surface color space: {} ({})",
                      colorspace,
                      colorspace == EGL_GL_COLORSPACE_SRGB_KHR ? "sRGB" :
                      colorspace == EGL_GL_COLORSPACE_LINEAR_KHR ? "LINEAR — driver downgraded the request" :
                      "unknown");
        }
    }
    return surf;
}

static void GL_APIENTRY gl_debug_callback(GLenum /*source*/, GLenum type, GLuint id,
                                          GLenum severity, GLsizei /*length*/,
                                          const GLchar* message, const void* /*user*/) {
    // KHR_debug — severity is HIGH/MEDIUM/LOW/NOTIFICATION; downgrade
    // notifications to debug noise but always surface errors.
    // (KHR_debug is core in ES 3.2 — tokens have no _KHR suffix.)
    const char* sev = "?";
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:         sev = "HIGH";   break;
        case GL_DEBUG_SEVERITY_MEDIUM:       sev = "MEDIUM"; break;
        case GL_DEBUG_SEVERITY_LOW:          sev = "LOW";    break;
        case GL_DEBUG_SEVERITY_NOTIFICATION: return;  // skip notifications
    }
    if (type == GL_DEBUG_TYPE_ERROR) {
        log::error(TAG, "GL[{}] id={} {}", sev, id, message);
    } else {
        log::warn(TAG, "GL[{}] id={} {}", sev, id, message);
    }
}

bool Rhi::init(const Config& config, platform::Platform& platform) {
    // 1. Get the EGL display (default device).
    m_impl->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_impl->egl_display == EGL_NO_DISPLAY) {
        log::error(TAG, "eglGetDisplay failed");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_impl->egl_display, &major, &minor)) {
        log::error(TAG, "eglInitialize failed (code 0x{:x})", eglGetError());
        return false;
    }
    log::info(TAG, "EGL {}.{} initialized", major, minor);

    // 2. Choose a config: RGBA8 + D24S8, ES 3.x, optional MSAA. Multisampling
    //    is configured per-pipeline on ES (via glRenderbufferStorageMultisample
    //    for offscreen targets) — for the default framebuffer we ask EGL for
    //    a multisample surface only when m_msaa_samples > 1.
    const EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(m_impl->egl_display, config_attrs, &m_impl->egl_config, 1, &num_configs)
        || num_configs < 1) {
        log::error(TAG, "eglChooseConfig found no suitable config (code 0x{:x})", eglGetError());
        return false;
    }

    // 3. Create a GLES 3.1 context. KHR_create_context lets us request a
    //    debug context when validation is enabled. We target 3.1 (not 3.2)
    //    because the engine doesn't currently use any 3.2-exclusive feature
    //    — compute, SSBOs, indirect draws, MSAA, KHR_debug-as-extension,
    //    and dynamically uniform sampler arrays are all already in 3.1.
    //    3.1 also covers the Android emulator and the long tail of older
    //    Mali / Adreno devices that report 3.1 even on recent Android.
    const EGLint ctx_flags = config.enable_validation ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0;
    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 1,
        EGL_CONTEXT_FLAGS_KHR,         ctx_flags,
        EGL_NONE
    };
    m_impl->egl_context = eglCreateContext(m_impl->egl_display, m_impl->egl_config,
                                           EGL_NO_CONTEXT, ctx_attrs);
    if (m_impl->egl_context == EGL_NO_CONTEXT) {
        log::error(TAG, "eglCreateContext for ES 3.1 failed (code 0x{:x})", eglGetError());
        return false;
    }

    // 4. Create the window surface against the platform's ANativeWindow.
    //    Request an sRGB framebuffer via EGL_KHR_gl_colorspace so the
    //    swapchain auto-encodes shader-output linear values to sRGB on
    //    write — matches the Vulkan backend (which picks an sRGB
    //    swapchain format) and keeps brightness consistent across
    //    backends. Without this the shader's linear color goes to the
    //    panel uncorrected and the whole scene looks ~2× too dark.
    //
    //    The extension has been required by GLES 3.0+ for years; we
    //    still gracefully fall back to a default (linear) surface if
    //    sRGB isn't advertised, so an ancient device just gets the
    //    pre-fix dim look rather than a hard failure.
    m_native_window = platform.native_window_handle();
    if (!m_native_window) {
        log::error(TAG, "GLES init: platform has no native window handle yet");
        return false;
    }
    m_impl->egl_surface = create_window_surface_srgb(m_impl->egl_display,
                                                      m_impl->egl_config,
                                                      m_native_window);
    if (m_impl->egl_surface == EGL_NO_SURFACE) {
        log::error(TAG, "eglCreateWindowSurface failed (code 0x{:x})", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(m_impl->egl_display, m_impl->egl_surface,
                        m_impl->egl_surface, m_impl->egl_context)) {
        log::error(TAG, "eglMakeCurrent failed (code 0x{:x})", eglGetError());
        return false;
    }

    // 5. Query back the surface size so handle_resize doesn't need to be the
    //    first call. Width/height come from the ANativeWindow.
    EGLint width = 0, height = 0;
    eglQuerySurface(m_impl->egl_display, m_impl->egl_surface, EGL_WIDTH,  &width);
    eglQuerySurface(m_impl->egl_display, m_impl->egl_surface, EGL_HEIGHT, &height);
    m_extent = { static_cast<u32>(width), static_cast<u32>(height) };

    // 6. KHR_debug: hook the GL callback when validation is on. KHR_debug
    //    is an extension on ES 3.1 (core only in 3.2) but every shipping
    //    Android driver exposes it; we just check the extension string.
    //
    //    GL_DEBUG_OUTPUT_SYNCHRONOUS would force the driver to invoke our
    //    callback on the thread that made the offending GL call before
    //    the call returns — which inserts a CPU/GPU sync on every single
    //    GL call and ruins frame time. Leave it off; async delivery
    //    (callback may arrive on a driver thread, possibly out of order)
    //    is plenty for catching errors during development.
    if (config.enable_validation) {
        const auto* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (ext && std::strstr(ext, "GL_KHR_debug")) {
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(&gl_debug_callback, nullptr);
        }
    }

    // GLES 3.1 core forbids draw calls without a non-zero VAO bound. The
    // engine doesn't otherwise care about VAOs (vertex format is re-applied
    // per draw in command_list.cpp::apply_vertex_input), so we just keep
    // one global default VAO bound for the context's lifetime.
    glGenVertexArrays(1, &m_default_vao);
    glBindVertexArray(m_default_vao);

    // Allocate the draw-info UBO that feeds per-draw `firstInstance` into
    // vertex shaders (GLES baseInstance emulation). Bound once at slot 31
    // and re-written between draws by draw_indexed_indirect / draw_indexed.
    // Single-uint payload; the std140 layout pads it to 16 bytes.
    glGenBuffers(1, &m_impl->draw_info_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, m_impl->draw_info_ubo);
    {
        u32 zero[4] = {0, 0, 0, 0};
        glBufferData(GL_UNIFORM_BUFFER, sizeof(zero), zero, GL_DYNAMIC_DRAW);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER,
                     PipelineLayoutRecord::kDrawInstanceInfoSlot,
                     m_impl->draw_info_ubo);

    // Default pixel unpack alignment is 4 — fine for RGBA8 but corrupts
    // R8 / RG8 uploads whose row width isn't a multiple of 4 (the font
    // atlas is one such consumer: glyph bitmaps come in arbitrary widths).
    // Our staging buffers are tight-packed (no row padding) so alignment
    // 1 is the right value across the board.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    log::info(TAG, "GLES context created: {}x{}, version={}, vendor={}, renderer={}",
              width, height,
              reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // Sanity-check the binding pools we depend on. The engine uses
    // UBO slots up to 31 (push-constant UBO + draw-info UBO at the
    // top of the pool; lower slots for shadow/env/per-pipeline UBOs)
    // and a small number of SSBO slots (currently 1 per pipeline at
    // binding 0). ES 3.1 minimums are SSBO=4 and UBO=24, so a device
    // sitting at spec-min would fail silently on slots 24-31. Mali
    // Valhall + Adreno 6xx+ go well above the minimum; if a low-end
    // chip ever exposes <32 UBO slots we want a hard error at boot
    // rather than mysterious missing-data renders later.
    {
        GLint max_ubo = 0, max_ssbo_vs = 0, max_ssbo_fs = 0, max_combined_tex = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS,          &max_ubo);
        glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS,      &max_ssbo_vs);
        glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS,    &max_ssbo_fs);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,      &max_combined_tex);
        log::info(TAG,
                  "GL limits: UBO bindings={}, SSBO blocks (vs/fs)={}/{}, combined tex units={}",
                  max_ubo, max_ssbo_vs, max_ssbo_fs, max_combined_tex);
        if (max_ubo < 32) {
            log::error(TAG,
                       "GL_MAX_UNIFORM_BUFFER_BINDINGS={} < 32 required by the engine "
                       "(push-constant UBO uses slot 30, draw-info UBO uses slot 31). "
                       "Bindings above the limit are silently dropped.", max_ubo);
            return false;
        }
        if (max_ssbo_vs < 1) {
            log::error(TAG,
                       "GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS={} < 1 required for "
                       "instance / bone SSBOs.", max_ssbo_vs);
            return false;
        }
    }
    return true;
}

void Rhi::shutdown() {
    if (m_impl->egl_display == EGL_NO_DISPLAY) return;

    // Destroy any GL objects still live in the record tables (resources a
    // caller forgot to destroy). Done while the EGL context is still current
    // — the eglDestroyContext below would otherwise orphan them. Descriptor
    // set / layout records hold no GL names, so they need no GL teardown.
    for (auto& rec : m_impl->buffers) {
        if (rec.name != 0) { glDeleteBuffers(1, &rec.name); rec.name = 0; }
    }
    for (auto& rec : m_impl->textures) {
        if (rec.name != 0) { glDeleteTextures(1, &rec.name); rec.name = 0; }
    }
    for (auto& rec : m_impl->samplers) {
        if (rec.name != 0) { glDeleteSamplers(1, &rec.name); rec.name = 0; }
    }
    for (auto& rec : m_impl->shader_modules) {
        if (rec.shader != 0) { glDeleteShader(rec.shader); rec.shader = 0; }
    }
    for (auto& rec : m_impl->pl_records) {
        if (rec.push_constant_ubo != 0) { glDeleteBuffers(1, &rec.push_constant_ubo); rec.push_constant_ubo = 0; }
    }
    for (auto& rec : m_impl->pipeline_records) {
        if (rec.program != 0) { glDeleteProgram(rec.program); rec.program = 0; }
    }

    if (m_default_vao != 0) {
        glBindVertexArray(0);
        glDeleteVertexArrays(1, &m_default_vao);
        m_default_vao = 0;
    }
    if (m_impl->draw_info_ubo != 0) {
        glDeleteBuffers(1, &m_impl->draw_info_ubo);
        m_impl->draw_info_ubo = 0;
    }

    eglMakeCurrent(m_impl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_impl->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_impl->egl_display, m_impl->egl_surface);
        m_impl->egl_surface = EGL_NO_SURFACE;
    }
    if (m_impl->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_impl->egl_display, m_impl->egl_context);
        m_impl->egl_context = EGL_NO_CONTEXT;
    }
    eglTerminate(m_impl->egl_display);
    m_impl->egl_display = EGL_NO_DISPLAY;
    m_native_window = nullptr;
}

// ── Frame ─────────────────────────────────────────────────────────────

CommandList Rhi::begin_frame() {
    // GL has no command-buffer object; recording is immediate-mode against
    // the current context. The CommandList carries `this` as its backend
    // handle, which gles/command_list.cpp uses to reach into record tables
    // when it needs to translate handles.
    if (m_impl->egl_surface == EGL_NO_SURFACE) return {};
    // Per-frame once-upload bookkeeping: clear the synced-this-frame flag
    // on every host-visible buffer so the first sync of each draw path
    // actually pushes its CPU shadow. See sync_buffer_to_gpu() for why.
    for (auto& rec : m_impl->buffers) {
        rec.synced_this_frame = false;
    }
    return CommandList(*this, this);
}

void Rhi::begin_rendering() {
    // Default framebuffer + full-extent viewport. Clear color + depth so the
    // frame starts from a known state. Pre-render passes (shadow, etc.) bind
    // their own FBO via cmd.begin_rendering(); this is the main pass.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<GLsizei>(m_extent.width), static_cast<GLsizei>(m_extent.height));
    // glClear honors scissor and color mask; a stale scissor (from ImGui or
    // the HUD overlay last frame) would leave un-cleared pixels and produce
    // ghost frames. Reset both before clearing.
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Rhi::end_frame() {
    if (m_impl->egl_surface != EGL_NO_SURFACE) {
        eglSwapBuffers(m_impl->egl_display, m_impl->egl_surface);
    }
    m_frame_index = (m_frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
}

CommandList Rhi::begin_oneshot() {
    // GL has no separate command buffer abstraction; the returned
    // CommandList records straight into the active context.
    return CommandList(*this, this);
}

void Rhi::end_oneshot(CommandList& /*cmd*/) {
    glFinish();
}

void Rhi::handle_resize(u32 width, u32 height) {
    m_extent = { width, height };
    // Surface size updates automatically on Android — the next eglMakeCurrent
    // / eglSwapBuffers picks up the new ANativeWindow extent. Nothing more
    // to do here unless we have an offscreen MSAA target (not yet).
}

void Rhi::set_vsync(bool enabled) {
    // eglSwapInterval(1) locks presentation to vblank (vsync on); 0 means
    // present as fast as the compositor allows. Applies from the next
    // eglSwapBuffers; no surface rebuild needed. Requires a current context.
    if (m_impl->egl_display == EGL_NO_DISPLAY) return;
    eglSwapInterval(m_impl->egl_display, enabled ? 1 : 0);
}

void Rhi::recreate_surface(platform::Platform& platform) {
    // Android handoff: the previous ANativeWindow has been invalidated.
    // Drop the old EGL surface, build a new one against the platform's
    // current window, and re-bind the context.
    if (m_impl->egl_display == EGL_NO_DISPLAY) return;

    eglMakeCurrent(m_impl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_impl->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_impl->egl_display, m_impl->egl_surface);
        m_impl->egl_surface = EGL_NO_SURFACE;
    }

    m_native_window = platform.native_window_handle();
    if (!m_native_window) {
        log::warn(TAG, "recreate_surface: platform has no window — context detached");
        return;
    }

    m_impl->egl_surface = create_window_surface_srgb(m_impl->egl_display,
                                                      m_impl->egl_config,
                                                      m_native_window);
    if (m_impl->egl_surface == EGL_NO_SURFACE) {
        log::error(TAG, "recreate_surface: eglCreateWindowSurface failed (0x{:x})", eglGetError());
        return;
    }
    if (!eglMakeCurrent(m_impl->egl_display, m_impl->egl_surface,
                        m_impl->egl_surface, m_impl->egl_context)) {
        log::error(TAG, "recreate_surface: eglMakeCurrent failed (0x{:x})", eglGetError());
        return;
    }

    EGLint width = 0, height = 0;
    eglQuerySurface(m_impl->egl_display, m_impl->egl_surface, EGL_WIDTH,  &width);
    eglQuerySurface(m_impl->egl_display, m_impl->egl_surface, EGL_HEIGHT, &height);
    m_extent = { static_cast<u32>(width), static_cast<u32>(height) };
    log::info(TAG, "GLES surface recreated: {}x{}", width, height);
}

void Rhi::wait_idle() { glFinish(); }

// Surface format reporting — until the EGL config is selected at init,
// these return Undefined. The pipeline factories should tolerate that.
TextureFormat Rhi::swapchain_format() const { return TextureFormat::R8G8B8A8_SRGB; }

void Rhi::set_base_instance(u32 base) {
    if (base == m_impl->last_base_instance) return;
    m_impl->last_base_instance = base;
    if (m_impl->draw_info_ubo == 0) return;
    // std140 a uint as the only field still occupies 16 bytes for alignment.
    u32 payload[4] = {base, 0, 0, 0};
    glBindBuffer(GL_UNIFORM_BUFFER, m_impl->draw_info_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(payload), payload);
}
TextureFormat Rhi::depth_format()     const { return TextureFormat::D32_SFLOAT; }

// ── Resource factories (TODO) ─────────────────────────────────────────

BufferHandle Rhi::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0 || desc.usage == BufferUsage::None) {
        log::error(TAG, "create_buffer: invalid desc (size={}, usage=0)", desc.size);
        return {};
    }

    GLuint name = 0;
    glGenBuffers(1, &name);
    if (name == 0) {
        log::error(TAG, "glGenBuffers failed");
        return {};
    }

    const GLenum target = to_gl_buffer_target(desc.usage);
    const GLenum hint   = to_gl_buffer_usage_hint(desc.memory);
    glBindBuffer(target, name);
    glBufferData(target, static_cast<GLsizeiptr>(desc.size), nullptr, hint);

    u32 idx = acquire_slot(m_impl->buffers, m_impl->buffers_free);
    auto& rec = m_impl->buffers[idx];
    rec.name   = name;
    rec.target = target;
    rec.size   = desc.size;
    if (desc.memory == MemoryUsage::HostSequential || desc.memory == MemoryUsage::HostRandom) {
        rec.shadow.assign(desc.size, 0);
        rec.shadow_dirty = true;  // initial contents are zero; nothing in GL yet
    }
    u32 gen = bump_generation(rec);
    return BufferHandle{idx, gen};
}

void Rhi::destroy_buffer(BufferHandle h) {
    if (!h.is_valid() || h.index >= m_impl->buffers.size()) return;
    auto& rec = m_impl->buffers[h.index];
    if (rec.generation != h.generation) return;
    if (rec.name != 0) {
        glDeleteBuffers(1, &rec.name);
        rec.name = 0;
    }
    rec.shadow.clear();
    rec.shadow.shrink_to_fit();
    rec.shadow_dirty = false;
    rec.generation = 0;
    m_impl->buffers_free.push_back(h.index);
}

void* Rhi::mapped_ptr(BufferHandle h) const {
    if (!h.is_valid() || h.index >= m_impl->buffers.size()) return nullptr;
    auto& rec = const_cast<BufferRecord&>(m_impl->buffers[h.index]);
    if (rec.generation != h.generation) return nullptr;
    if (rec.shadow.empty()) return nullptr;  // GpuOnly buffer — not host-visible
    // Caller is about to write; mark dirty so the next use re-uploads, and
    // clear the per-frame sync flag so the next sync_buffer_to_gpu actually
    // pushes the new data instead of short-circuiting.
    rec.shadow_dirty = true;
    rec.synced_this_frame = false;
    return rec.shadow.data();
}

void Rhi::sync_buffer_to_gpu(BufferHandle h) {
    if (!h.is_valid() || h.index >= m_impl->buffers.size()) return;
    auto& rec = m_impl->buffers[h.index];
    if (rec.generation != h.generation || rec.shadow.empty()) return;
    // Once-per-frame upload. Without this guard the GLES backend
    // re-pushed the full shadow on every draw / bind / copy that
    // touched the buffer, which for the static-mesh path meant
    // 4 partition iterations × (384 KB instance + 80 KB indirect)
    // = ~1.8 MB of glBufferSubData per frame on the bus, and the
    // resulting implicit GPU syncs cost ~25 ms / frame on Adreno.
    // Clearing the flag at begin_frame and after mapped_ptr writes
    // keeps a fresh write visible to the next draw.
    if (rec.synced_this_frame) return;
    // We can't reliably know whether the shadow has been written to since
    // the last sync — Vulkan's VMA persistent mapping lets consumers cache
    // the pointer from a single mapped_ptr() call and write through it
    // every frame. mapped_ptr() can't observe those writes, so the
    // shadow_dirty flag (set only on mapped_ptr) misses them. Always
    // upload at sync time. (Bandwidth optimization for later: explicit
    // flush API or EXT_buffer_storage when available.)
    glBindBuffer(rec.target, rec.name);
    glBufferSubData(rec.target, 0, static_cast<GLsizeiptr>(rec.size), rec.shadow.data());
    rec.shadow_dirty = false;
    rec.synced_this_frame = true;
}

TextureHandle Rhi::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.format == TextureFormat::Undefined) {
        log::error(TAG, "create_texture: invalid desc ({}x{}, format=0)", desc.width, desc.height);
        return {};
    }

    GLuint name = 0;
    glGenTextures(1, &name);
    if (name == 0) {
        log::error(TAG, "glGenTextures failed");
        return {};
    }

    const auto fmt = to_gl_format(desc.format);
    if (fmt.internal == 0) {
        log::error(TAG, "create_texture: unsupported format {}", static_cast<u32>(desc.format));
        glDeleteTextures(1, &name);
        return {};
    }

    GLenum target = GL_TEXTURE_2D;
    if (desc.type == TextureType::TextureCube) {
        target = GL_TEXTURE_CUBE_MAP;
    } else if (desc.type == TextureType::Texture2DArray) {
        target = GL_TEXTURE_2D_ARRAY;
    }

    glBindTexture(target, name);

    // Storage is allocated immutably (matches Vulkan's vkBindImageMemory
    // pattern). Uploads come later via glTexSubImage in CommandList's
    // copy_buffer_to_image.
    const GLsizei mips   = static_cast<GLsizei>(desc.mip_levels > 0 ? desc.mip_levels : 1);
    const GLsizei w      = static_cast<GLsizei>(desc.width);
    const GLsizei h      = static_cast<GLsizei>(desc.height);
    const GLsizei layers = static_cast<GLsizei>(desc.array_layers);

    if (target == GL_TEXTURE_2D) {
        glTexStorage2D(target, mips, fmt.internal, w, h);
    } else if (target == GL_TEXTURE_CUBE_MAP) {
        glTexStorage2D(target, mips, fmt.internal, w, h);
    } else { // GL_TEXTURE_2D_ARRAY
        glTexStorage3D(target, mips, fmt.internal, w, h, layers);
    }

    u32 idx = acquire_slot(m_impl->textures, m_impl->textures_free);
    auto& rec = m_impl->textures[idx];
    rec.name            = name;
    rec.target          = target;
    rec.internal_format = fmt.internal;
    rec.width           = desc.width;
    rec.height          = desc.height;
    rec.layers          = desc.array_layers;
    rec.mips            = mips;
    u32 gen = bump_generation(rec);
    return TextureHandle{idx, gen};
}

void Rhi::destroy_texture(TextureHandle h) {
    if (!h.is_valid() || h.index >= m_impl->textures.size()) return;
    auto& rec = m_impl->textures[h.index];
    if (rec.generation != h.generation) return;
    if (rec.name != 0) {
        glDeleteTextures(1, &rec.name);
        rec.name = 0;
    }
    rec.generation = 0;
    m_impl->textures_free.push_back(h.index);
}

SamplerHandle Rhi::create_sampler(const SamplerDesc& desc) {
    GLuint name = 0;
    glGenSamplers(1, &name);
    if (name == 0) {
        log::error(TAG, "glGenSamplers failed");
        return {};
    }

    glSamplerParameteri(name, GL_TEXTURE_WRAP_S, to_gl_wrap(desc.address_u));
    glSamplerParameteri(name, GL_TEXTURE_WRAP_T, to_gl_wrap(desc.address_v));
    glSamplerParameteri(name, GL_TEXTURE_WRAP_R, to_gl_wrap(desc.address_w));
    glSamplerParameteri(name, GL_TEXTURE_MAG_FILTER,
                        desc.mag_filter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(name, GL_TEXTURE_MIN_FILTER,
                        to_gl_min_filter(desc.min_filter, desc.mipmap_mode, desc.max_lod));
    glSamplerParameterf(name, GL_TEXTURE_MIN_LOD, desc.min_lod);
    glSamplerParameterf(name, GL_TEXTURE_MAX_LOD, desc.max_lod);

    if (desc.compare_enable) {
        glSamplerParameteri(name, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        // CompareOp::Less → GL_LESS, etc. Default Less matches the engine's
        // shadow-PCF expectation; expand the switch if other ops show up.
        glSamplerParameteri(name, GL_TEXTURE_COMPARE_FUNC, GL_LESS);
    }

    u32 idx = acquire_slot(m_impl->samplers, m_impl->samplers_free);
    auto& rec = m_impl->samplers[idx];
    rec.name = name;
    u32 gen = bump_generation(rec);
    return SamplerHandle{idx, gen};
}

void Rhi::destroy_sampler(SamplerHandle h) {
    if (!h.is_valid() || h.index >= m_impl->samplers.size()) return;
    auto& rec = m_impl->samplers[h.index];
    if (rec.generation != h.generation) return;
    if (rec.name != 0) {
        glDeleteSamplers(1, &rec.name);
        rec.name = 0;
    }
    rec.generation = 0;
    m_impl->samplers_free.push_back(h.index);
}

ShaderModuleHandle Rhi::create_shader_module(std::span<const u8> source) {
    // GLES expects null-terminated GLSL source. We rely on the asset packer
    // shipping a `.glsl` variant alongside each `.spv` — produced by
    // spirv-cross at desktop build time. The first byte of the bundle
    // indicates the stage (V=vertex, F=fragment) so we don't need a
    // separate descriptor:
    //
    //   bytes[0]      = stage tag ('V' / 'F')
    //   bytes[1]      = '\n'
    //   bytes[2..end] = GLSL source (null-terminated by the packer)
    if (source.size() < 4) {
        log::error(TAG, "create_shader_module: source too small ({} bytes)", source.size());
        return {};
    }
    GLenum stage = GL_VERTEX_SHADER;
    switch (source[0]) {
        case 'V': stage = GL_VERTEX_SHADER;   break;
        case 'F': stage = GL_FRAGMENT_SHADER; break;
        default:
            log::error(TAG, "create_shader_module: unknown stage tag '{}'", source[0]);
            return {};
    }

    GLuint shader = glCreateShader(stage);
    if (shader == 0) {
        log::error(TAG, "glCreateShader failed");
        return {};
    }

    const GLchar* src    = reinterpret_cast<const GLchar*>(source.data() + 2);
    const GLint   length = static_cast<GLint>(source.size() - 2);
    glShaderSource(shader, 1, &src, &length);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        // Drivers may return 0 even when compilation failed. Always
        // reserve at least one byte so `buf.data()` is non-null and the
        // log call doesn't dereference a nullptr.
        std::vector<char> buf(static_cast<usize>(log_len > 0 ? log_len : 1), '\0');
        if (log_len > 0) {
            glGetShaderInfoLog(shader, log_len, nullptr, buf.data());
        }
        log::error(TAG, "shader compile failed (stage={}, log_len={}): {}",
                   stage == GL_VERTEX_SHADER ? "VS" : "FS",
                   log_len, buf.data());
        glDeleteShader(shader);
        return {};
    }

    u32 idx = acquire_slot(m_impl->shader_modules, m_impl->shader_modules_free);
    auto& rec = m_impl->shader_modules[idx];
    rec.shader = shader;
    rec.stage  = stage;
    u32 gen = bump_generation(rec);
    return ShaderModuleHandle{idx, gen};
}

void Rhi::destroy_shader_module(ShaderModuleHandle h) {
    if (!h.is_valid() || h.index >= m_impl->shader_modules.size()) return;
    auto& rec = m_impl->shader_modules[h.index];
    if (rec.generation != h.generation) return;
    if (rec.shader != 0) {
        glDeleteShader(rec.shader);
        rec.shader = 0;
    }
    rec.generation = 0;
    m_impl->shader_modules_free.push_back(h.index);
}

DescriptorSetLayoutHandle Rhi::create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) {
    u32 idx = acquire_slot(m_impl->dsl_records, m_impl->dsl_free);
    auto& rec = m_impl->dsl_records[idx];
    rec.bindings.assign(desc.bindings.begin(), desc.bindings.end());
    u32 gen = bump_generation(rec);
    return DescriptorSetLayoutHandle{idx, gen};
}

void Rhi::destroy_descriptor_set_layout(DescriptorSetLayoutHandle h) {
    if (!h.is_valid() || h.index >= m_impl->dsl_records.size()) return;
    auto& rec = m_impl->dsl_records[h.index];
    if (rec.generation != h.generation) return;
    rec.bindings.clear();
    rec.generation = 0;
    m_impl->dsl_free.push_back(h.index);
}

DescriptorSetHandle Rhi::allocate_descriptor_set(DescriptorSetLayoutHandle layout, u32 /*variable_count*/) {
    if (!layout.is_valid()) return {};
    u32 idx = acquire_slot(m_impl->dset_records, m_impl->dset_free);
    auto& rec = m_impl->dset_records[idx];
    rec.layout = layout;
    rec.bindings.clear();
    u32 gen = bump_generation(rec);
    return DescriptorSetHandle{idx, gen};
}

void Rhi::update_descriptor_set(DescriptorSetHandle h, std::span<const WriteDescriptor> writes) {
    if (!h.is_valid() || h.index >= m_impl->dset_records.size()) return;
    auto& rec = m_impl->dset_records[h.index];
    if (rec.generation != h.generation) return;

    for (const auto& w : writes) {
        // Find existing binding slot or append.
        auto it = std::find_if(rec.bindings.begin(), rec.bindings.end(),
            [&](const DescriptorSetRecord::Binding& b) {
                return b.binding == w.binding && b.array_element == w.array_element;
            });
        DescriptorSetRecord::Binding* bp = nullptr;
        if (it != rec.bindings.end()) {
            bp = &*it;
        } else {
            rec.bindings.emplace_back();
            bp = &rec.bindings.back();
        }
        bp->binding       = w.binding;
        bp->array_element = w.array_element;
        bp->type          = w.type;
        bp->texture       = w.texture;
        bp->sampler       = w.sampler;
        bp->buffer        = w.buffer;
        bp->buffer_offset = w.buffer_offset;
        bp->buffer_range  = w.buffer_range;
    }
}

void Rhi::free_descriptor_set(DescriptorSetHandle h) {
    if (!h.is_valid() || h.index >= m_impl->dset_records.size()) return;
    auto& rec = m_impl->dset_records[h.index];
    if (rec.generation != h.generation) return;
    rec.bindings.clear();
    rec.layout = {};
    rec.generation = 0;
    m_impl->dset_free.push_back(h.index);
}

PipelineLayoutHandle Rhi::create_pipeline_layout(const PipelineLayoutDesc& desc) {
    u32 idx = acquire_slot(m_impl->pl_records, m_impl->pl_free);
    auto& rec = m_impl->pl_records[idx];
    rec.set_layouts.assign(desc.set_layouts.begin(), desc.set_layouts.end());
    rec.push_constants.assign(desc.push_constants.begin(), desc.push_constants.end());

    // If any push-constant range exists, allocate a small UBO at the
    // reserved slot. The CommandList will glBufferSubData into it on
    // push_constants() and that UBO stays bound while the pipeline is
    // active.
    rec.push_constant_size = 0;
    for (const auto& pc : rec.push_constants) {
        if (pc.offset + pc.size > rec.push_constant_size) {
            rec.push_constant_size = pc.offset + pc.size;
        }
    }
    if (rec.push_constant_size > 0) {
        glGenBuffers(1, &rec.push_constant_ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, rec.push_constant_ubo);
        glBufferData(GL_UNIFORM_BUFFER, rec.push_constant_size, nullptr, GL_DYNAMIC_DRAW);
    }

    u32 gen = bump_generation(rec);
    return PipelineLayoutHandle{idx, gen};
}

void Rhi::destroy_pipeline_layout(PipelineLayoutHandle h) {
    if (!h.is_valid() || h.index >= m_impl->pl_records.size()) return;
    auto& rec = m_impl->pl_records[h.index];
    if (rec.generation != h.generation) return;
    if (rec.push_constant_ubo != 0) {
        glDeleteBuffers(1, &rec.push_constant_ubo);
        rec.push_constant_ubo = 0;
    }
    rec.set_layouts.clear();
    rec.push_constants.clear();
    rec.push_constant_size = 0;
    rec.generation = 0;
    m_impl->pl_free.push_back(h.index);
}

PipelineHandle Rhi::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    // 1. Link the program from the supplied shader stages.
    GLuint program = glCreateProgram();
    if (program == 0) {
        log::error(TAG, "glCreateProgram failed");
        return {};
    }
    for (const auto& s : desc.stages) {
        if (!s.module.is_valid() || s.module.index >= m_impl->shader_modules.size()) continue;
        const auto& mod = m_impl->shader_modules[s.module.index];
        if (mod.generation != s.module.generation) continue;
        glAttachShader(program, mod.shader);
    }
    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<char> buf(static_cast<usize>(log_len > 0 ? log_len : 1), '\0');
        if (log_len > 0) {
            glGetProgramInfoLog(program, log_len, nullptr, buf.data());
        }
        log::error(TAG, "program link failed (stages={}, log_len={}): {}",
                   desc.stages.size(), log_len, buf.data());
        glDeleteProgram(program);
        return {};
    }

    // 2. Record the rest of the pipeline state. Vertex format gets applied
    //    per-draw because GL ES needs the active VBO bound to set up
    //    glVertexAttribPointer.
    u32 idx = acquire_slot(m_impl->pipeline_records, m_impl->pipeline_free);
    auto& rec = m_impl->pipeline_records[idx];
    rec.program = program;
    rec.layout  = desc.layout;
    rec.topology      = desc.topology;
    rec.rasterizer    = desc.rasterizer;
    rec.depth_stencil = desc.depth_stencil;
    if (!desc.blend_attachments.empty()) rec.blend = desc.blend_attachments[0];
    rec.multisample   = desc.multisample;

    rec.vertex_bindings.assign(desc.vertex_input.bindings.begin(),
                               desc.vertex_input.bindings.end());
    rec.vertex_attrs.clear();
    rec.vertex_attrs.reserve(desc.vertex_input.attributes.size());
    for (const auto& a : desc.vertex_input.attributes) {
        rec.vertex_attrs.push_back({ a.location, a.binding, a.offset, a.format });
    }

    u32 gen = bump_generation(rec);
    return PipelineHandle{idx, gen};
}

void Rhi::destroy_pipeline(PipelineHandle h) {
    if (!h.is_valid() || h.index >= m_impl->pipeline_records.size()) return;
    auto& rec = m_impl->pipeline_records[h.index];
    if (rec.generation != h.generation) return;
    if (rec.program != 0) {
        glDeleteProgram(rec.program);
        rec.program = 0;
    }
    rec.vertex_bindings.clear();
    rec.vertex_attrs.clear();
    rec.generation = 0;
    m_impl->pipeline_free.push_back(h.index);
}

// ── Record accessors (used by CommandList in command_list.cpp) ───────

const Rhi::BufferRecord* Rhi::buffer_record(BufferHandle h) const {
    return detail::lookup(m_impl->buffers, h);
}
const Rhi::TextureRecord* Rhi::texture_record(TextureHandle h) const {
    return detail::lookup(m_impl->textures, h);
}
const Rhi::SamplerRecord* Rhi::sampler_record(SamplerHandle h) const {
    return detail::lookup(m_impl->samplers, h);
}
const Rhi::DescriptorSetRecord* Rhi::descriptor_set_record(DescriptorSetHandle h) const {
    return detail::lookup(m_impl->dset_records, h);
}
const Rhi::DescriptorSetLayoutRecord* Rhi::descriptor_set_layout_record(DescriptorSetLayoutHandle h) const {
    return detail::lookup(m_impl->dsl_records, h);
}
const Rhi::PipelineLayoutRecord* Rhi::pipeline_layout_record(PipelineLayoutHandle h) const {
    return detail::lookup(m_impl->pl_records, h);
}
const Rhi::PipelineRecord* Rhi::pipeline_record(PipelineHandle h) const {
    return detail::lookup(m_impl->pipeline_records, h);
}

} // namespace uldum::rhi
