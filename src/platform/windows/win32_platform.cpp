#include "platform/windows/win32_platform.h"
#include "core/log.h"

#include <cstdlib>
#include <filesystem>

namespace uldum::platform {

static constexpr const char* TAG = "Platform";

std::unique_ptr<Platform> Platform::create() {
    return std::make_unique<Win32Platform>();
}

Win32Platform::~Win32Platform() {
    shutdown();
}

bool Win32Platform::init(const Config& config) {
    m_hinstance = GetModuleHandle(nullptr);

    // Enable per-monitor DPI awareness (Windows 10 1703+)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = m_hinstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"UldumWindowClass";

    if (!RegisterClassExW(&wc)) {
        log::error(TAG, "Failed to register window class");
        return false;
    }

    // Scale requested size by monitor DPI
    HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi_x = 96, dpi_y = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    f32 scale = static_cast<f32>(dpi_x) / 96.0f;

    LONG scaled_w = static_cast<LONG>(config.width * scale);
    LONG scaled_h = static_cast<LONG>(config.height * scale);

    // Adjust window rect so the client area matches the scaled size
    RECT rect = {0, 0, scaled_w, scaled_h};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_x);

    int window_width  = rect.right - rect.left;
    int window_height = rect.bottom - rect.top;

    int title_len = MultiByteToWideChar(CP_UTF8, 0, config.title.data(),
                                         static_cast<int>(config.title.size()), nullptr, 0);
    std::wstring wide_title(title_len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, config.title.data(),
                        static_cast<int>(config.title.size()), wide_title.data(), title_len);

    m_hwnd = CreateWindowExW(
        0,
        L"UldumWindowClass",
        wide_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        window_width, window_height,
        nullptr, nullptr, m_hinstance, this
    );

    if (!m_hwnd) {
        log::error(TAG, "Failed to create window");
        return false;
    }

    // Store actual pixel dimensions (what Vulkan uses)
    RECT client;
    GetClientRect(m_hwnd, &client);
    m_width  = static_cast<u32>(client.right);
    m_height = static_cast<u32>(client.bottom);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    log::info(TAG, "Window created: {}x{} (DPI scale {:.0f}%)", m_width, m_height, scale * 100.0f);
    return true;
}

void Win32Platform::shutdown() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_hinstance) {
        UnregisterClassW(L"UldumWindowClass", m_hinstance);
        m_hinstance = nullptr;
    }
}

bool Win32Platform::poll_events() {
    // Clear per-frame deltas
    m_input.mouse_dx = 0;
    m_input.mouse_dy = 0;
    m_input.scroll_delta = 0;
    m_input.mouse_left_pressed = false;
    m_input.mouse_left_released = false;
    m_input.mouse_right_pressed = false;
    m_input.mouse_right_released = false;

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_quit = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !m_quit;
}

bool Win32Platform::was_resized() {
    bool r = m_resized;
    m_resized = false;
    return r;
}

std::vector<std::string> Win32Platform::list_files(std::string_view prefix) const {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path dir(std::string{prefix});
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        out.push_back(entry.path().filename().string());
    }
    return out;
}

void Win32Platform::set_cursor_visible(bool visible) {
    if (visible == m_cursor_visible) return;   // idempotent
    // ShowCursor maintains an internal counter; each FALSE decrements,
    // TRUE increments. We only ever transition once per state change
    // so the counter stays balanced. The actual hide/show only takes
    // effect when the counter crosses 0 / -1.
    ShowCursor(visible ? TRUE : FALSE);
    m_cursor_visible = visible;
}

void Win32Platform::set_fullscreen(bool fullscreen) {
    if (!m_hwnd || fullscreen == m_fullscreen) return;  // idempotent

    if (fullscreen) {
        // Save the current windowed placement so we can restore it later,
        // then strip the window chrome and size to the monitor the window
        // is on. Borderless-fullscreen (no exclusive mode-set): plays nice
        // with alt-tab and avoids display-mode churn.
        GetWindowPlacement(m_hwnd, &m_windowed_placement);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            LONG style = GetWindowLong(m_hwnd, GWL_STYLE);
            SetWindowLong(m_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(m_hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right  - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        // Restore the bordered style and the saved windowed placement. The
        // resulting WM_SIZE sets m_resized → swapchain rebuilds.
        LONG style = GetWindowLong(m_hwnd, GWL_STYLE);
        SetWindowLong(m_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(m_hwnd, &m_windowed_placement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    m_fullscreen = fullscreen;
}

std::string Win32Platform::user_data_dir(std::string_view key) const {
    // %LOCALAPPDATA% is shared across programs, so append `key` to keep this
    // program's data separate. Device-local (not roaming). The caller appends
    // only a filename.
    char* base = nullptr;
    size_t len = 0;
    std::string dir;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") == 0 && base) {
        dir = std::string(base) + "\\" + std::string(key);
        free(base);
    } else {
        dir = std::string(key);  // last-resort fallback (relative to CWD)
    }
    return dir;
}

f32 Win32Platform::ui_scale() const {
    if (!m_hwnd) return 1.0f;
    UINT dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) return 1.0f;
    // Microsoft's DIP baseline (1/96 inch per unit). Gives integer
    // ui_scale at the common Windows scale settings: 100% → 1.0,
    // 200% → 2.0, 300% → 3.0. 150%/125%/175% land at 1.5/1.25/1.75 —
    // not integer, but uncommon enough to live with. A 48-dp slot
    // renders at ~half inch on every Windows scale.
    return static_cast<f32>(dpi) / 96.0f;
}

LRESULT CALLBACK Win32Platform::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Win32Platform* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
        self = static_cast<Win32Platform*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Win32Platform*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // Forward to message hook (e.g. ImGui input handling)
    if (self->m_message_hook) {
        if (self->m_message_hook(hwnd, msg, wparam, lparam)) {
            return 0;
        }
    }

    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE: {
        u32 w = LOWORD(lparam);
        u32 h = HIWORD(lparam);
        if (w > 0 && h > 0 && (w != self->m_width || h != self->m_height)) {
            self->m_width  = w;
            self->m_height = h;
            self->m_resized = true;
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        // Alt (VK_MENU) and any key while Alt is held arrive as
        // WM_SYSKEYDOWN / WM_SYSKEYUP, not WM_KEYDOWN / WM_KEYUP.
        // Handle them through the same key-state update so Alt-modified
        // bindings work — but still fall through to DefWindowProc for
        // the SYS variants so the OS keeps Alt-F4, F10 menu activation,
        // and similar system shortcuts.
        const bool pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        const bool is_sys  = (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP);
        // Always set key_letter for A-Z
        if (wparam >= 'A' && wparam <= 'Z') {
            self->m_input.key_letter[wparam - 'A'] = pressed;
        }
        switch (wparam) {
        case VK_ESCAPE: self->m_input.key_escape = pressed; break;
        case VK_F1:     self->m_input.key_f1 = pressed; break;
        case VK_F2:     self->m_input.key_f2 = pressed; break;
        case VK_F3:     self->m_input.key_f3 = pressed; break;
        case VK_UP:     self->m_input.key_up = pressed; break;
        case VK_DOWN:   self->m_input.key_down = pressed; break;
        case VK_LEFT:   self->m_input.key_left = pressed; break;
        case VK_RIGHT:  self->m_input.key_right = pressed; break;
        case VK_SHIFT:  self->m_input.key_shift = pressed; break;
        case VK_CONTROL: self->m_input.key_ctrl = pressed; break;
        case VK_MENU:   self->m_input.key_alt = pressed; break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            self->m_input.key_num[wparam - '0'] = pressed; break;
        }
        // Consume normal key events; let SYS variants fall through to
        // DefWindowProc so Alt-F4, F10 menu, etc. still work.
        if (is_sys) break;
        return 0;
    }

    case WM_MOUSEMOVE: {
        f32 nx = static_cast<f32>(GET_X_LPARAM(lparam));
        f32 ny = static_cast<f32>(GET_Y_LPARAM(lparam));
        // Accumulate the delta across every WM_MOUSEMOVE that arrives
        // between two poll_events calls. Plain assignment used to lose
        // all but the final hop when the OS coalesced multiple moves
        // into one poll (visible as sluggish camera pan on fast mouse
        // motion). dx/dy reset to 0 at the top of poll_events.
        self->m_input.mouse_dx += nx - self->m_input.mouse_x;
        self->m_input.mouse_dy += ny - self->m_input.mouse_y;
        self->m_input.mouse_x = nx;
        self->m_input.mouse_y = ny;
        return 0;
    }

    // Mouse buttons. SetCapture on the first button-down so a drag that
    // leaves the window still routes its WM_*BUTTONUP back here —
    // otherwise the up event goes to whichever window the cursor is
    // over and our button state stays stuck-down (drag-select keeps
    // following the cursor, etc.). Release capture only when every
    // button is up; otherwise a multi-button drag with one button
    // released would prematurely lose capture.
    case WM_LBUTTONDOWN:
        if (!self->m_input.mouse_left && !self->m_input.mouse_right && !self->m_input.mouse_middle) SetCapture(hwnd);
        self->m_input.mouse_left = true;  self->m_input.mouse_left_pressed = true;  return 0;
    case WM_LBUTTONUP:
        self->m_input.mouse_left = false; self->m_input.mouse_left_released = true;
        if (!self->m_input.mouse_right && !self->m_input.mouse_middle) ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        if (!self->m_input.mouse_left && !self->m_input.mouse_right && !self->m_input.mouse_middle) SetCapture(hwnd);
        self->m_input.mouse_right = true;  self->m_input.mouse_right_pressed = true;  return 0;
    case WM_RBUTTONUP:
        self->m_input.mouse_right = false; self->m_input.mouse_right_released = true;
        if (!self->m_input.mouse_left && !self->m_input.mouse_middle) ReleaseCapture();
        return 0;
    case WM_MBUTTONDOWN:
        if (!self->m_input.mouse_left && !self->m_input.mouse_right && !self->m_input.mouse_middle) SetCapture(hwnd);
        self->m_input.mouse_middle = true;  return 0;
    case WM_MBUTTONUP:
        self->m_input.mouse_middle = false;
        if (!self->m_input.mouse_left && !self->m_input.mouse_right) ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL:
        self->m_input.scroll_delta = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) / 120.0f;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace uldum::platform
