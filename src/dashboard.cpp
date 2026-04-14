#include "dashboard.h"

#ifdef _WIN32

#include "diagnostics.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace dashboard {
namespace {

class DashboardWindow;
DashboardWindow* g_instance = nullptr;

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], size);
    return out;
}

void append_metric(std::ostringstream& out, const char* label, int64_t value) {
    out << label << ": ";
    if (value >= 0) {
        out << value << " ms";
    } else {
        out << "n/a";
    }
    out << '\n';
}

std::string format_snapshot(const diagnostics::DiagnosticsSnapshot& snapshot) {
    std::ostringstream out;
    out << "Local TTS Diagnostics Dashboard\n";
    out << "================================\n\n";

    out << "Current state: " << diagnostics::live_state_name(snapshot.state) << "\n\n";

    const auto& session = snapshot.latest_session;
    out << "Latest session summary\n";
    out << "----------------------\n";
    out << "session id: " << session.session_id << '\n';
    append_metric(out, "recording duration", session.recording_duration_ms);
    append_metric(out, "wav write duration", session.wav_write_duration_ms);
    append_metric(out, "whisper duration", session.whisper_duration_ms);
    append_metric(out, "correction duration", session.correction_duration_ms);
    append_metric(out, "sanitization duration", session.sanitization_duration_ms);
    append_metric(out, "paste duration", session.paste_duration_ms);
    append_metric(out, "recording-stop to paste", session.from_recording_stop_to_paste_ms);
    append_metric(out, "total hotkey-to-paste", session.total_end_to_end_duration_ms);
    out << "correction applied: " << (session.correction_applied ? "true" : "false") << '\n';
    out << "segmented: " << (session.segmented ? "true" : "false") << '\n';
    out << "segment count: " << session.segment_count << '\n';
    out << "paste outcome: " << diagnostics::paste_outcome_name(session.paste_outcome) << "\n\n";

    out << "Recent events\n";
    out << "-------------\n";
    if (snapshot.recent_events.empty()) {
        out << "(none)\n";
        return out.str();
    }

    const std::size_t max_rows = 180;
    const std::size_t row_count = (std::min)(max_rows, snapshot.recent_events.size());
    const std::size_t start = snapshot.recent_events.size() - row_count;
    for (std::size_t i = start; i < snapshot.recent_events.size(); ++i) {
        const auto& e = snapshot.recent_events[i];
        out << "t=" << e.timestamp_ms << " ms"
            << " | s=" << e.session_id
            << " | stage=" << diagnostics::stage_name(e.stage)
            << " | kind=" << diagnostics::event_kind_name(e.kind);
        if (e.duration_ms >= 0) {
            out << " | elapsed=" << e.duration_ms << " ms";
        }
        if (e.has_success) {
            out << " | success=" << (e.success ? "true" : "false");
        }
        if (!e.message.empty()) {
            out << " | note=" << e.message;
        }
        out << '\n';
    }
    return out.str();
}

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
                                    920,
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
    bool create_contents() noexcept {
        try {
            text_hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                                         L"EDIT",
                                         L"",
                                         WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                             ES_READONLY | WS_VSCROLL,
                                         0,
                                         0,
                                         100,
                                         100,
                                         hwnd_,
                                         nullptr,
                                         GetModuleHandleW(nullptr),
                                         nullptr);
            if (!text_hwnd_) {
                return false;
            }
            SendMessageW(text_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            diagnostics::register_dashboard_window(hwnd_);
            refresh_text();
            return true;
        } catch (...) {
            return false;
        }
    }

    void layout_contents() noexcept {
        if (!text_hwnd_) {
            return;
        }
        RECT rc{};
        if (!GetClientRect(hwnd_, &rc)) {
            return;
        }
        const int padding = 8;
        MoveWindow(text_hwnd_,
                   padding,
                   padding,
                   (std::max)(0, (rc.right - rc.left) - (padding * 2)),
                   (std::max)(0, (rc.bottom - rc.top) - (padding * 2)),
                   TRUE);
    }

    void refresh_text() noexcept {
        try {
            if (!text_hwnd_) {
                return;
            }
            const auto snapshot = diagnostics::get_snapshot(300);
            const auto text = format_snapshot(snapshot);
            const auto wide = utf8_to_wide(text);
            SetWindowTextW(text_hwnd_, wide.c_str());
        } catch (...) {
        }
    }

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

        const UINT update_msg = diagnostics::dashboard_update_message();
        if (msg == WM_CREATE) {
            return self->create_contents() ? 0 : -1;
        }
        if (msg == WM_SIZE) {
            self->layout_contents();
            return 0;
        }
        if (msg == update_msg) {
            self->refresh_text();
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

    HWND hwnd_ = nullptr;
    HWND text_hwnd_ = nullptr;
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
