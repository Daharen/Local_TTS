#include "live_mode.h"

#include "audio_capture.h"
#include "llm_correction.h"
#include "paths.h"
#include "text_injection.h"
#include "whisper_runner.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <cwchar>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>

namespace {

constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 40;
constexpr UINT kTrayId = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_TRANSCRIBE_DONE = WM_APP + 2;
constexpr UINT ID_TRAY_EXIT = 1001;

std::string now_stamp_for_file() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return out.str();
}

std::string now_stamp_readable() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

class LiveModeApp {
public:
    explicit LiveModeApp(bool debug_console) : debug_console_(debug_console) {}

    int run() {
        if (!create_window()) {
            return 1;
        }
        set_state(L"Idle");
        if (!debug_console_) {
            ShowWindow(window_, SW_HIDE);
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        shutdown();
        return 0;
    }

private:
    struct LogPayload {
        bool correction_enabled = false;
        bool correction_applied = false;
        bool paste_done = false;
        bool paste_skipped = false;
        bool paste_failed = false;
        std::string wav_path;
        std::string raw_transcript;
        std::string corrected_transcript;
        std::string transcribe_error;
        std::string correction_error;
        std::string paste_error;
    };

    void debug_line(const std::string& line, bool as_error = false) const {
        if (!debug_console_) {
            return;
        }
        (as_error ? std::cerr : std::cout) << line << '\n';
    }

    bool create_window() {
        HINSTANCE inst = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &LiveModeApp::window_proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"LocalTTSLiveModeWindow";

        if (!RegisterClassExW(&wc)) {
            return false;
        }

        window_ = CreateWindowExW(0,
                                  wc.lpszClassName,
                                  L"Local TTS Live",
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  nullptr,
                                  nullptr,
                                  inst,
                                  this);
        if (!window_) {
            return false;
        }

        SetTimer(window_, kTimerId, kTimerMs, nullptr);
        return create_tray_icon();
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        LiveModeApp* self = reinterpret_cast<LiveModeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<LiveModeApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        if (!self) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        switch (msg) {
            case WM_TIMER:
                self->on_timer();
                return 0;
            case WM_COMMAND:
                if (LOWORD(wparam) == ID_TRAY_EXIT) {
                    self->request_exit();
                }
                return 0;
            case WM_TRAYICON:
                if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
                    self->show_tray_menu();
                }
                return 0;
            case WM_TRANSCRIBE_DONE:
                self->on_transcribe_done();
                return 0;
            case WM_CLOSE:
                self->request_exit();
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    void on_timer() {
        if (shutting_down_ || transcribing_.load()) {
            return;
        }

        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool should_record = ctrl && alt;

        if (should_record && !recording_) {
            start_recording();
            return;
        }

        if (recording_ && !should_record) {
            stop_recording_and_transcribe();
        }
    }

    void start_recording() {
        target_window_ = GetForegroundWindow();
        if (!capture_.start()) {
            set_state(L"Idle");
            return;
        }
        recording_ = true;
        MessageBeep(MB_OK);
        set_state(L"Recording");
        debug_line("[CAPTURE_START]");
    }

    void stop_recording_and_transcribe() {
        recording_ = false;
        capture_.stop();
        MessageBeep(MB_ICONASTERISK);
        set_state(L"Transcribing");
        debug_line("[CAPTURE_STOP]");

        transcribing_.store(true);

        worker_ = std::thread([this]() {
            const auto large_root = get_large_data_root();
            const auto wav_path = large_root / "temp" / "live" / (now_stamp_for_file() + ".wav");
            const auto log_path = large_root / "output" / "live_transcripts" / "session.txt";

            LogPayload log;
            log.correction_enabled = is_correction_enabled();
            log.wav_path = wav_path.string();
            debug_line("[WAV_PATH] " + log.wav_path);

            if (!capture_.write_wav(wav_path)) {
                log.transcribe_error = "Failed to write WAV file.";
                debug_line("[WHISPER_ERROR] " + log.transcribe_error, true);
            } else {
                transcribe_file_to_string(wav_path, log.raw_transcript, log.transcribe_error);
                if (!log.transcribe_error.empty()) {
                    debug_line("[WHISPER_ERROR] " + log.transcribe_error, true);
                } else {
                    debug_line("[WHISPER_RAW] " + log.raw_transcript);
                }
            }

            std::string output_text = log.raw_transcript;
            debug_line(std::string("[LLM_ENABLED] ") + (log.correction_enabled ? "true" : "false"));

            if (log.correction_enabled && !log.raw_transcript.empty()) {
                CorrectionRunInfo info;
                if (correct_transcript_text_with_info(
                        log.raw_transcript, log.corrected_transcript, log.correction_error, &info) &&
                    !log.corrected_transcript.empty()) {
                    output_text = log.corrected_transcript;
                    log.correction_applied = true;
                    debug_line("[LLM_INPUT] " + log.raw_transcript);
                    debug_line("[LLM_OUTPUT] " + log.corrected_transcript);
                    debug_line("[LLM_APPLIED] true");
                } else {
                    debug_line("[LLM_INPUT] " + log.raw_transcript);
                    if (!log.corrected_transcript.empty()) {
                        debug_line("[LLM_OUTPUT] " + log.corrected_transcript);
                    }
                    debug_line("[LLM_FAILED] " + log.correction_error, true);
                    debug_line("[LLM_APPLIED] false");
                }
            } else if (!log.raw_transcript.empty()) {
                debug_line("[LLM_APPLIED] false");
            }

            if (!output_text.empty()) {
                HWND current = GetForegroundWindow();
                const bool target_valid = target_window_ && IsWindow(target_window_) && IsWindowVisible(target_window_);

                if (!target_valid) {
                    log.paste_skipped = true;
                    log.paste_error = "Target window is no longer valid/visible.";
                    debug_line("[PASTE_SKIPPED] " + log.paste_error, true);
                } else if (current != target_window_) {
                    log.paste_skipped = true;
                    log.paste_error = "Focus changed; paste skipped to avoid disruptive window changes.";
                    debug_line("[PASTE_SKIPPED] " + log.paste_error, true);
                } else {
                    std::string inject_error;
                    if (!inject_text_via_clipboard_paste(target_window_, output_text, inject_error)) {
                        log.paste_failed = true;
                        log.paste_error = inject_error;
                        debug_line("[PASTE_FAILED] " + inject_error, true);
                    } else {
                        log.paste_done = true;
                        debug_line("[PASTE_DONE]");
                    }
                }
            } else if (log.transcribe_error.empty()) {
                log.paste_skipped = true;
                log.paste_error = "No transcript text available for paste.";
                debug_line("[PASTE_SKIPPED] " + log.paste_error);
            }

            append_log(log_path, log);
            PostMessageW(window_, WM_TRANSCRIBE_DONE, 0, 0);
        });
    }

    void append_log(const std::filesystem::path& log_path, const LogPayload& log) {
        std::filesystem::create_directories(log_path.parent_path());
        std::ofstream out(log_path, std::ios::app);
        if (!out) {
            return;
        }

        out << "[TIMESTAMP] " << now_stamp_readable() << "\n";
        out << "[WAV_PATH] " << log.wav_path << "\n";
        out << "[CORRECTION_ENABLED] " << (log.correction_enabled ? "true" : "false") << "\n";

        if (!log.raw_transcript.empty()) {
            out << "[RAW_WHISPER] " << log.raw_transcript << "\n";
        }
        if (!log.corrected_transcript.empty()) {
            out << "[CORRECTED_TEXT] " << log.corrected_transcript << "\n";
        }
        out << "[CORRECTION_APPLIED] " << (log.correction_applied ? "true" : "false") << "\n";

        if (!log.correction_error.empty()) {
            out << "[CORRECTION_FAILED] " << log.correction_error << "\n";
        }
        if (!log.transcribe_error.empty()) {
            out << "[TRANSCRIBE_ERROR] " << log.transcribe_error << "\n";
        }
        if (log.paste_done) {
            out << "[PASTE_DONE]\n";
        }
        if (log.paste_skipped) {
            out << "[PASTE_SKIPPED] " << log.paste_error << "\n";
        }
        if (log.paste_failed) {
            out << "[PASTE_FAILED] " << log.paste_error << "\n";
        }
        out << "\n";
    }

    void on_transcribe_done() {
        if (worker_.joinable()) {
            worker_.join();
        }
        transcribing_.store(false);
        if (!shutting_down_) {
            set_state(L"Idle");
        }
    }

    bool create_tray_icon() {
        std::memset(&tray_icon_, 0, sizeof(tray_icon_));
        tray_icon_.cbSize = sizeof(tray_icon_);
        tray_icon_.hWnd = window_;
        tray_icon_.uID = kTrayId;
        tray_icon_.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
        tray_icon_.uCallbackMessage = WM_TRAYICON;
        tray_icon_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy_s(tray_icon_.szTip, L"Local TTS - Idle");

        return Shell_NotifyIconW(NIM_ADD, &tray_icon_) == TRUE;
    }

    void set_state(const wchar_t* state) {
        state_text_ = state;
        std::string state_marker = "Transcribing";
        if (std::wcscmp(state, L"Idle") == 0) {
            state_marker = "Idle";
        } else if (std::wcscmp(state, L"Recording") == 0) {
            state_marker = "Recording";
        }
        debug_line("[LIVE_STATE] " + state_marker);
        if (!window_) {
            return;
        }

        tray_icon_.uFlags = NIF_TIP;
        std::wstring tip = L"Local TTS - ";
        tip += state_text_;
        wcsncpy_s(tray_icon_.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &tray_icon_);
    }

    void show_tray_menu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void request_exit() {
        shutting_down_ = true;
        debug_line("[SHUTDOWN]");
        KillTimer(window_, kTimerId);
        if (recording_) {
            recording_ = false;
            capture_.stop();
        }
        if (worker_.joinable()) {
            worker_.join();
            transcribing_.store(false);
        }
        DestroyWindow(window_);
    }

    void shutdown() {
        capture_.cleanup();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (tray_icon_.hWnd) {
            Shell_NotifyIconW(NIM_DELETE, &tray_icon_);
            tray_icon_.hWnd = nullptr;
        }
    }

    HWND window_ = nullptr;
    HWND target_window_ = nullptr;
    std::thread worker_;
    AudioCapture capture_;
    NOTIFYICONDATAW tray_icon_{};
    std::wstring state_text_ = L"Idle";
    std::atomic<bool> transcribing_{false};
    bool recording_ = false;
    bool shutting_down_ = false;
    bool debug_console_ = false;
};

}  // namespace

int run_live_mode(bool debug_console) {
    HWND console = GetConsoleWindow();
    if (console && !debug_console) {
        ShowWindow(console, SW_HIDE);
    }

    LiveModeApp app(debug_console);
    return app.run();
}

#else

#include <iostream>

int run_live_mode(bool) {
    std::cerr << "Live mode is Windows-only.\n";
    return 1;
}

#endif
