#pragma once

#include "platform/platform.h"

#include <Windows.h>
#include <windowsx.h>
#include <shellscalingapi.h>

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

    // Windows reports DPI (dots per inch). Convert to px-per-dp using
    // the dp baseline of 160 DPI. At 100% Windows scale the monitor
    // is assumed 96 DPI, so px-per-dp = 0.6; at 150% scale (144 DPI),
    // 0.9; at 200% (192 DPI), 1.2. The HUD looks the same physical
    // size as on an Android device at the same density bucket.
    f32 ui_scale() const override;

    void* native_window_handle() const override { return m_hwnd; }
    void* native_instance_handle() const override { return m_hinstance; }

    std::vector<std::string> list_files(std::string_view prefix) const override;

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
