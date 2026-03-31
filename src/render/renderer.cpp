#include "render/renderer.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "core/log.h"

namespace uldum::render {

static constexpr const char* TAG = "Render";
static bool s_first_frame = true;

bool Renderer::init(rhi::VulkanRhi& rhi) {
    m_rhi = &rhi;
    log::info(TAG, "Renderer initialized (stub) — render graph, materials, terrain pending");
    return true;
}

void Renderer::shutdown() {
    m_rhi = nullptr;
    log::info(TAG, "Renderer shut down (stub)");
}

void Renderer::begin_frame() {
    if (s_first_frame) {
        log::trace(TAG, "begin_frame (stub) — will build render graph here");
    }
}

void Renderer::end_frame() {
    if (s_first_frame) {
        log::trace(TAG, "end_frame (stub) — will submit render graph here");
        s_first_frame = false;
    }
}

} // namespace uldum::render
