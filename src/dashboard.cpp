#include "dashboard.h"

#ifdef _WIN32

namespace dashboard {
namespace {

class DashboardWindow;
DashboardWindow* g_instance = nullptr;

class DashboardWindow {
public:
    explicit DashboardWindow(bool debug_console) : debug_console_(debug_console) {}

    bool create(HWND owner) noexcept {
        try {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &DashboardWindow::window_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"LocalTTSDiagnosticsDashboard";
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassExW(&wc);

            hwnd_ = CreateWindowExW(0,
                                    wc.lpszClassName,
                                    L"Local TTS Dashboard",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    800,
                                    500,
                                    owner,
                                    nullptr,
                                    inst,
                                    this);
            return hwnd_ != nullptr;
        } catch (...) {
            return false;
        }
    }

    void bring_to_front() noexcept {
        if (!hwnd_) {
            return;
        }
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }

    HWND hwnd() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        DashboardWindow* self = reinterpret_cast<DashboardWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<DashboardWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        if (!self) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        if (msg == WM_CREATE) {
            return 0;
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY) {
            if (g_instance == self) {
                g_instance = nullptr;
                delete self;
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    [[maybe_unused]] bool debug_console_ = false;
};

}  // namespace

bool show_dashboard_window(HWND owner, bool debug_console) noexcept {
    try {
        if (g_instance && IsWindow(g_instance->hwnd())) {
            g_instance->bring_to_front();
            return true;
        }
        auto* window = new DashboardWindow(debug_console);
        if (!window->create(owner)) {
            delete window;
            return false;
        }
        g_instance = window;
        return true;
    } catch (...) {
        return false;
    }
}

void close_dashboard_window() noexcept {
    if (g_instance && IsWindow(g_instance->hwnd())) {
        DestroyWindow(g_instance->hwnd());
    }
}

}  // namespace dashboard

#else

namespace dashboard {

bool show_dashboard_window(HWND, bool) noexcept {
    return false;
}

void close_dashboard_window() noexcept {}

}  // namespace dashboard

#endif
