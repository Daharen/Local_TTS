#include "dashboard.h"

#include "diagnostics.h"

#include <sstream>
#include <string>

#ifdef _WIN32

namespace dashboard {
namespace {

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
            RegisterClassExW(&wc);

            hwnd_ = CreateWindowExW(0,
                                    wc.lpszClassName,
                                    L"Local TTS Diagnostics",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    900,
                                    640,
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
            self->on_create(hwnd);
            return 0;
        }
        if (msg == WM_SIZE) {
            self->on_size(LOWORD(lparam), HIWORD(lparam));
            return 0;
        }
        if (msg == diagnostics::dashboard_update_message()) {
            self->refresh();
            return 0;
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY) {
            diagnostics::unregister_dashboard_window(hwnd);
            if (g_instance == self) {
                g_instance = nullptr;
                delete self;
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void on_create(HWND hwnd) noexcept {
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                                L"EDIT",
                                L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                0,
                                0,
                                0,
                                0,
                                hwnd,
                                nullptr,
                                GetModuleHandleW(nullptr),
                                nullptr);
        diagnostics::register_dashboard_window(hwnd);
        refresh();
    }

    void on_size(int width, int height) noexcept {
        if (edit_) {
            MoveWindow(edit_, 8, 8, width - 16, height - 16, TRUE);
        }
    }

    std::wstring to_wide(const std::string& input) {
        if (input.empty()) {
            return L"";
        }
        const int len = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
        if (len <= 1) {
            return L"";
        }
        std::wstring out(static_cast<std::size_t>(len - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, &out[0], len);
        return out;
    }

    void refresh() noexcept {
        if (!edit_) {
            return;
        }
        const auto snap = diagnostics::get_snapshot(220);
        std::ostringstream out;
        out << "Live State: " << diagnostics::live_state_name(snap.state) << "\r\n\r\n";

        const auto& s = snap.latest_session;
        out << "Latest Session\r\n";
        out << "  id: " << s.session_id << "\r\n";
        out << "  recording_ms: " << s.recording_duration_ms << "\r\n";
        out << "  wav_write_ms: " << s.wav_write_duration_ms << "\r\n";
        out << "  whisper_ms: " << s.whisper_duration_ms << "\r\n";
        out << "  correction_ms: " << s.correction_duration_ms << "\r\n";
        out << "  sanitize_ms: " << s.sanitization_duration_ms << "\r\n";
        out << "  paste_ms: " << s.paste_duration_ms << "\r\n";
        out << "  recstop_to_paste_ms: " << s.from_recording_stop_to_paste_ms << "\r\n";
        out << "  hotkey_to_paste_ms: " << s.total_end_to_end_duration_ms << "\r\n";
        out << "  correction_applied: " << (s.correction_applied ? "true" : "false") << "\r\n";
        out << "  segmented: " << (s.segmented ? "true" : "false") << " (" << s.segment_count << ")\r\n";
        out << "  paste_outcome: " << diagnostics::paste_outcome_name(s.paste_outcome) << "\r\n\r\n";

        out << "Recent Events\r\n";
        for (const auto& e : snap.recent_events) {
            out << "  t=" << e.timestamp_ms << "ms"
                << " session=" << e.session_id
                << " stage=" << diagnostics::stage_name(e.stage)
                << " kind=" << diagnostics::event_kind_name(e.kind);
            if (e.duration_ms >= 0) {
                out << " elapsed=" << e.duration_ms << "ms";
            }
            if (e.has_success) {
                out << " success=" << (e.success ? "true" : "false");
            }
            if (!e.message.empty()) {
                out << " note=" << e.message;
            }
            out << "\r\n";
        }

        const std::wstring text = to_wide(out.str());
        SetWindowTextW(edit_, text.c_str());
    }

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    bool debug_console_ = false;
};

DashboardWindow* g_instance = nullptr;

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
