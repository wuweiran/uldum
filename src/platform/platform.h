#pragma once

#include "core/types.h"

#include <memory>
#include <string_view>

namespace uldum::platform {

struct Config {
    std::string_view title = "Uldum Engine";
    u32 width  = 1280;
    u32 height = 720;
};

struct InputState {
    bool key_escape = false;
};

class Platform {
public:
    virtual ~Platform() = default;

    virtual bool init(const Config& config) = 0;
    virtual void shutdown() = 0;

    // Returns false when the OS requests quit (WM_QUIT, etc.)
    virtual bool poll_events() = 0;

    virtual const InputState& input() const = 0;

    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    virtual bool was_resized() = 0;

    // Vulkan surface creation needs these
    virtual void* native_window_handle() const = 0;
    virtual void* native_instance_handle() const = 0;

    static std::unique_ptr<Platform> create();
};

} // namespace uldum::platform
