#pragma once

// 16c-iii.d — selection circles. One pre-built thin ring mesh, drawn at
// world z=0 (ground) per selected unit, scaled to the unit's selection
// radius and colored by relationship to the local player. Flat — no
// terrain conforming; good enough for level / gently-sloped test maps.
// Full ground-decal projection (terrain-conforming, slope-aware) is a
// later upgrade.

#include "core/types.h"
#include "simulation/handle_types.h"

#include <vector>

typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace uldum::rhi        { class VulkanRhi; }
namespace uldum::render     { class Camera; }
namespace uldum::simulation { struct World; }
namespace uldum::map        { struct TerrainData; }

namespace uldum::render {

class SelectionCircles {
public:
    SelectionCircles();
    ~SelectionCircles();
    SelectionCircles(const SelectionCircles&) = delete;
    SelectionCircles& operator=(const SelectionCircles&) = delete;

    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Draw one ring for each unit in `selected`. Green when the unit is
    // owned by `local_player`, red otherwise. Call inside the active
    // render pass, after the 3D scene draws. Safe to call with an empty
    // selection or a null terrain (no-op).
    //
    // `alpha` is the sub-tick interpolation factor (0..1) so rings follow
    // smoothly-moving units; matches `Renderer::draw`'s alpha.
    // `terrain` is sampled per ring vertex so rings conform to ground
    // slope instead of clipping through hills.
    void draw(VkCommandBuffer cmd,
              const Camera& camera,
              const simulation::World& world,
              const map::TerrainData* terrain,
              const std::vector<simulation::Unit>& selected,
              simulation::Player local_player,
              f32 alpha);

    // Opaque state — declared in selection_circles.cpp so the header
    // stays free of vulkan.h / VMA.
    struct Impl;
private:
    Impl* m_impl = nullptr;
};

} // namespace uldum::render
