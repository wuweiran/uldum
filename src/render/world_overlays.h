#pragma once

// World overlays — the unified ground-decal renderer. Replaces the
// previous SelectionCircles + AbilityIndicators classes; everything
// that draws on top of the terrain (selection rings, cast range ring,
// cast curve, reticle, AoE shapes, target-unit ring, future build-
// placement ghost, debug gizmos) goes through here.
//
// Design follows the WC3 model: shape lives in the texture, geometry
// is a thin substrate. Three primitive generators cover all current
// indicators:
//   • add_ring   — closed annular ribbon (selection, range, target-unit)
//   • add_path   — open ribbon along a sample curve (cast curve, AoE line)
//   • add_quad   — flat ground decal (reticle, AoE circle)
//   • add_cone   — variable-angle wedge fan (AoE cone)
//
// Each primitive picks a TextureId — one of a small set of runtime-
// generated default decals (ring stroke, soft circle, donut, gradient
// curve, cone wedge, line). Maps can later override by shipping a KTX2
// at a path declared in hud.json; the consumer code stays unchanged.
//
// All draws share one pipeline (`engine/shaders/world_overlay.{vert,frag}`)
// and one VBO; the per-draw texture switch is a `vkCmdBindDescriptorSets`
// call between draws.

#include "core/types.h"
#include "simulation/handle_types.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <span>
#include <string_view>
#include <vector>

typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

class WorldOverlays {
public:
    // Texture identity — names the customizable overlay slots. Each
    // slot owns its own VkImage + descriptor set, populated lazily on
    // the first set_texture() call. Slots are unbound at init: any
    // add_* call referencing an unbound slot is silently dropped, so
    // a map that doesn't supply textures simply gets no overlays.
    // The engine-default / map-override merging mechanism (planned)
    // will populate every slot at app init from a known engine path.
    enum class TextureId : u32 {
        SelectionRing  = 0,  // ribbon — under selected units
        RangeRing,           // ribbon — caster's cast range
        TargetUnit,          // ribbon — snapped target unit
        CastCurve,           // ribbon — cast arrow (V-axis alpha gradient)
        Reticle,             // quad   — ground-target marker (donut)
        AoeCircle,           // quad   — AoE preview circle
        AoeLine,             // ribbon — AoE preview line
        AoeCone,             // fan    — AoE preview cone

        kCount
    };

    // Replace the procedural default at `id` with a runtime-loaded
    // texture. `path` resolves through the AssetManager (so map paths
    // and engine paths both work). On failure the existing texture is
    // kept and the call returns false. Safe to call any time after
    // init() — typically invoked from app code after the map's
    // hud.json is parsed.
    bool set_texture(TextureId id, std::string_view path);

    WorldOverlays();
    ~WorldOverlays();
    WorldOverlays(const WorldOverlays&) = delete;
    WorldOverlays& operator=(const WorldOverlays&) = delete;

    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Reset draw / vertex state for a new frame. Call once per frame
    // before any add_*, then `draw()` to flush.
    void begin_frame();

    // Closed annular ribbon centered at `center` with `radius` and
    // `thickness` (world units). `samples_per_ring` controls smoothness
    // (default 48 — same as the previous SelectionCircles default).
    // Vertices are terrain-flat at center.z; raise center.z slightly
    // before calling if you want to avoid z-fighting on slopes.
    //
    // Texture is sampled with U=0 at the inner rim, U=1 at the outer
    // rim, V wrapping around the ring. Color is the per-call tint
    // (pre-multiplied internally).
    void add_ring(glm::vec3 center, f32 radius, f32 thickness,
                  glm::vec4 color, TextureId tex,
                  u32 samples_per_ring = 48);

    // Open ribbon following the polyline in `samples`. Each segment is
    // a flat strip in the XY plane perpendicular to the segment's
    // direction; UV V runs from 0 at samples.front() to 1 at
    // samples.back(), U=0 inner edge, U=1 outer edge.
    void add_path(std::span<const glm::vec3> samples, f32 thickness,
                  glm::vec4 color, TextureId tex);

    // Axis-aligned ground quad of half-extent `half_extent` centered
    // at `center`. UV spans [0,1]² across the quad.
    void add_quad(glm::vec3 center, f32 half_extent,
                  glm::vec4 color, TextureId tex);

    // Variable-angle wedge fan from `origin` along `dir` (normalized
    // XY direction), spanning ±`half_angle` radians, with radial extent
    // `radius`. UV V=0 at origin, V=1 at rim; U wraps from 0 to 1
    // across the wedge angle. `segments` controls smoothness along
    // the angular sweep.
    void add_cone(glm::vec3 origin, glm::vec3 dir, f32 half_angle,
                  f32 radius, glm::vec4 color, TextureId tex,
                  u32 segments = 24);

    // Issue draws into `cmd` against `view_projection`. Call inside
    // the active render pass, after the 3D scene draws and before the
    // HUD. No-op if the draw list is empty.
    void draw(VkCommandBuffer cmd, const glm::mat4& view_projection);

    // Opaque state — declared in world_overlays.cpp so this header
    // stays free of vulkan.h / VMA.
    struct Impl;
private:
    Impl* m_impl = nullptr;
};

} // namespace uldum::render
