#pragma once

// Backend-agnostic entry point for the RHI.
//
// Consumer code should `#include "rhi/rhi.h"` and reference `rhi::Rhi`,
// `rhi::CommandList`, and the handle / desc types declared in this folder.
//
// At compile time, the backend is selected by `ULDUM_BACKEND_*` macros set
// by the build system. Each backend lives in `rhi/<name>/` and defines
// `class Rhi` with an identical public interface, plus backend-tied
// accessors (e.g. `Rhi::device()` on Vulkan, `Rhi::d3d_device()` on DX12)
// for backend-specific consumers like ImGui.
//
// Currently shipping backends:
//   • Vulkan 1.3 — Windows, Android, Linux
// Planned backends:
//   • OpenGL ES 3.2 — Android fallback, web (via WebGL)
//   • DX12 — Windows native
//   • Metal — macOS, iOS

#if defined(ULDUM_BACKEND_GLES)
#  include "rhi/gles/gles_rhi.h"
#elif defined(ULDUM_BACKEND_DX12)
#  include "rhi/dx12/dx12_rhi.h"
#elif defined(ULDUM_BACKEND_METAL)
#  include "rhi/metal/metal_rhi.h"
#else  // default: Vulkan
#  include "rhi/vulkan/vulkan_rhi.h"
#endif
