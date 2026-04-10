#include "platform/windows/win32_platform.h"
#include "core/log.h"

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

    // Convert title to wide string
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
    case WM_KEYUP: {
        bool pressed = (msg == WM_KEYDOWN);
        // Always set key_letter for A-Z
        if (wparam >= 'A' && wparam <= 'Z') {
            self->m_input.key_letter[wparam - 'A'] = pressed;
        }
        switch (wparam) {
        case VK_ESCAPE: self->m_input.key_escape = pressed; break;
        case 'W':       self->m_input.key_w = pressed; break;
        case 'A':       self->m_input.key_a = pressed; break;
        case 'S':       self->m_input.key_s = pressed; break;
        case 'D':       self->m_input.key_d = pressed; break;
        case 'Q':       self->m_input.key_q = pressed; break;
        case 'E':       self->m_input.key_e = pressed; break;
        case 'H':       self->m_input.key_h = pressed; break;
        case 'P':       self->m_input.key_p = pressed; break;
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
        return 0;
    }

    case WM_MOUSEMOVE: {
        f32 nx = static_cast<f32>(GET_X_LPARAM(lparam));
        f32 ny = static_cast<f32>(GET_Y_LPARAM(lparam));
        self->m_input.mouse_dx = nx - self->m_input.mouse_x;
        self->m_input.mouse_dy = ny - self->m_input.mouse_y;
        self->m_input.mouse_x = nx;
        self->m_input.mouse_y = ny;
        return 0;
    }

    case WM_LBUTTONDOWN: self->m_input.mouse_left = true;  self->m_input.mouse_left_pressed = true;  return 0;
    case WM_LBUTTONUP:   self->m_input.mouse_left = false; self->m_input.mouse_left_released = true; return 0;
    case WM_RBUTTONDOWN: self->m_input.mouse_right = true;  self->m_input.mouse_right_pressed = true;  return 0;
    case WM_RBUTTONUP:   self->m_input.mouse_right = false; self->m_input.mouse_right_released = true; return 0;
    case WM_MBUTTONDOWN: self->m_input.mouse_middle = true;  return 0;
    case WM_MBUTTONUP:   self->m_input.mouse_middle = false; return 0;

    case WM_MOUSEWHEEL:
        self->m_input.scroll_delta = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) / 120.0f;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace uldum::platform
