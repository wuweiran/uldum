#pragma once

// EditorOverlays — depth-tested 3D line renderer for editor visuals.
//
// Owns its own pipeline (line-list topology) so footprint indicators,
// selection circles, the terrain brush grid, and future region
// indicators all render through one path with correct occlusion against
// terrain and models. Each call queues a line or polyline; draw() emits
// the batch inside the active render pass, after the scene draw and
// before any 2D UI.
//
// Vertex format is position (vec3) + RGBA8 — no texture, no descriptor
// sets. Keep it lean: the editor's overlay language is line art.

#include "core/types.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <span>

typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::editor {

class EditorOverlays {
public:
    EditorOverlays();
    ~EditorOverlays();
    EditorOverlays(const EditorOverlays&) = delete;
    EditorOverlays& operator=(const EditorOverlays&) = delete;

    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Reset the vertex / draw-cmd buffers for a new frame. Call once
    // before any add_* call.
    void begin_frame();

    // Single line segment between a and b.
    void add_line(glm::vec3 a, glm::vec3 b, glm::vec4 color);

    // Open polyline through the sample points. With `closed = true`
    // an extra segment closes the loop back to samples[0].
    void add_polyline(std::span<const glm::vec3> samples, glm::vec4 color,
                      bool closed = false);

    // Emit the batched lines into `cmd` against `view_projection`.
    // Call inside the active render pass; safe to call when nothing
    // has been queued (no-op).
    void draw(VkCommandBuffer cmd, const glm::mat4& view_projection);

    struct Impl;
private:
    Impl* m_impl = nullptr;
};

} // namespace uldum::editor
