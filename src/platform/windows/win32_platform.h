#pragma once

#include "platform/platform.h"

#include <Windows.h>

namespace uldum::platform {

class Win32Platform final : public Platform {
public:
    Win32Platform() = default;
    ~Win32Platform() override;

    bool init(const Config& config) override;
    void shutdown() override;
    bool poll_events() override;

    const InputState& input() const override { return m_input; }

    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }
    bool was_resized() override;

    void* native_window_handle() const override { return m_hwnd; }
    void* native_instance_handle() const override { return m_hinstance; }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HWND      m_hwnd      = nullptr;
    HINSTANCE m_hinstance  = nullptr;
    u32       m_width      = 0;
    u32       m_height     = 0;
    bool      m_quit       = false;
    bool      m_resized    = false;
    InputState m_input     = {};
};

} // namespace uldum::platform
