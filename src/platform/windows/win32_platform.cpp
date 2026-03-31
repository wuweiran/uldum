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

    // Adjust window rect so the client area matches the requested size
    RECT rect = {0, 0, static_cast<LONG>(config.width), static_cast<LONG>(config.height)};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

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

    m_width  = config.width;
    m_height = config.height;

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    log::info(TAG, "Window created: {}x{}", m_width, m_height);
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
    m_input = {};
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
        if (wparam == VK_ESCAPE) self->m_input.key_escape = true;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace uldum::platform
