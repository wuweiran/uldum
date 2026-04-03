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
        switch (wparam) {
        case VK_ESCAPE: self->m_input.key_escape = pressed; break;
        case 'W':       self->m_input.key_w = pressed; break;
        case 'A':       self->m_input.key_a = pressed; break;
        case 'S':       self->m_input.key_s = pressed; break;
        case 'D':       self->m_input.key_d = pressed; break;
        case 'Q':       self->m_input.key_q = pressed; break;
        case 'E':       self->m_input.key_e = pressed; break;
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace uldum::platform
