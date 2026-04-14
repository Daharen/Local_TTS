#include "live_mode.h"

#include "audio_capture.h"
#include "dashboard.h"
#include "diagnostics.h"
#include "llm_correction.h"
#include "pipeline_debug.h"
#include "paths.h"
#include "text_injection.h"
#include "whisper_runner.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <cwchar>
#include <algorithm>
#include <cstdint>

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
constexpr UINT ID_TRAY_DASHBOARD = 1002;

std::string normalize_mode_label(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return mode == "notes" ? "notes" : "formatted";
}

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

std::string sanitize_payload_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c != '\0') {
            out.push_back(c);
        }
    }
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

std::string compact_excerpt(const std::string& text, std::size_t limit = 220) {
    if (text.size() <= limit) {
        return text;
    }
    if (limit < 8) {
        return text.substr(0, limit);
    }
    return text.substr(0, limit - 3) + "...";
}

int rough_word_count(const std::string& text) {
    int words = 0;
    bool in_word = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            in_word = false;
            continue;
        }
        if (!in_word) {
            ++words;
            in_word = true;
        }
    }
    return words;
}

std::string hwnd_to_string(HWND hwnd) {
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd);
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
        std::string correction_mode;
        std::string raw_transcript;
        std::string formatted_text;
        std::string transcribe_error;
        std::string correction_error;
        std::string llm_backend;
        std::string correction_raw_stdout;
        std::string correction_raw_stderr;
        std::string correction_sanitizer_reason;
        bool resident_attempted = false;
        bool resident_started = false;
        std::string resident_startup_error;
        std::string resident_endpoint_used;
        int resident_http_status = 0;
        std::string resident_error;
        std::string resident_server_exe;
        std::string resident_phase;
        int resident_total_budget_ms = 0;
        int resident_remaining_budget_ms = 0;
        int resident_attempt_timeout_ms = 0;
        int resident_request_count = 0;
        std::string resident_last_endpoint;
        int resident_last_status = 0;
        std::string resident_last_error;
        std::string resident_reset_reason;
        int resident_gpu_layers = 0;
        int resident_ctx_size = 0;
        int resident_threads = 0;
        bool oneshot_stderr_cuda_hint = false;
        bool correction_segmented = false;
        int correction_segment_count = 0;
        int correction_max_output_tokens = 0;
        std::string correction_failed_segments;
        std::string paste_error;
        std::string resident_args_excerpt;
        std::string resident_probe_response_excerpt;
        int whisper_text_chars = 0;
        int whisper_text_words = 0;
        int llm_output_chars = 0;
        int llm_output_words = 0;
    };

    void debug_line(const std::string& line, bool as_error = false) const {
        pipeline_debug::log("live_mode", line, as_error);
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
                if (LOWORD(wparam) == ID_TRAY_DASHBOARD) {
                    self->open_dashboard();
                } else if (LOWORD(wparam) == ID_TRAY_EXIT) {
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
        session_id_ = diagnostics::begin_session();
        diagnostics::diag_begin(session_id_, diagnostics::DiagnosticStage::RecordingStart, "recording requested");
        target_window_ = GetForegroundWindow();
        if (!capture_.start()) {
            diagnostics::diag_end(session_id_,
                                  diagnostics::DiagnosticStage::RecordingStart,
                                  "audio capture start failed",
                                  true,
                                  false);
            set_state(L"Idle");
            return;
        }
        recording_ = true;
        MessageBeep(MB_OK);
        set_state(L"Recording");
        debug_line("[CAPTURE_START]");
        diagnostics::diag_end(session_id_,
                              diagnostics::DiagnosticStage::RecordingStart,
                              "audio capture started",
                              true,
                              true);
    }

    void stop_recording_and_transcribe() {
        recording_ = false;
        capture_.stop();
        diagnostics::diag_point(session_id_, diagnostics::DiagnosticStage::RecordingStop, "recording stopped");
        diagnostics::set_recording_stop_time(session_id_);
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
            log.correction_mode = normalize_mode_label(get_correction_mode());
            log.correction_max_output_tokens = get_correction_max_output_tokens();
            log.wav_path = wav_path.string();
            debug_line("[WAV_PATH] " + log.wav_path);

            diagnostics::diag_begin(session_id_, diagnostics::DiagnosticStage::WavWrite, "write wav");
            if (!capture_.write_wav(wav_path)) {
                log.transcribe_error = "Failed to write WAV file.";
                debug_line("[WHISPER_ERROR] " + log.transcribe_error, true);
                diagnostics::diag_end(session_id_,
                                      diagnostics::DiagnosticStage::WavWrite,
                                      log.transcribe_error,
                                      true,
                                      false);
            } else {
                diagnostics::diag_end(session_id_,
                                      diagnostics::DiagnosticStage::WavWrite,
                                      "wav write completed",
                                      true,
                                      true);
                diagnostics::diag_begin(session_id_, diagnostics::DiagnosticStage::Whisper, "whisper begin");
                transcribe_file_to_string(wav_path, log.raw_transcript, log.transcribe_error);
                diagnostics::diag_end(session_id_,
                                      diagnostics::DiagnosticStage::Whisper,
                                      log.transcribe_error.empty() ? "whisper completed" : log.transcribe_error,
                                      true,
                                      log.transcribe_error.empty());
                if (!log.transcribe_error.empty()) {
                    debug_line("[WHISPER_ERROR] " + log.transcribe_error, true);
                } else {
                    debug_line("[RAW_WHISPER] " + log.raw_transcript);
                    log.whisper_text_chars = static_cast<int>(log.raw_transcript.size());
                    log.whisper_text_words = rough_word_count(log.raw_transcript);
                    debug_line("[HANDOFF_WHISPER_TO_LLM_BEGIN] chars=" + std::to_string(log.whisper_text_chars) +
                               " words=" + std::to_string(log.whisper_text_words) +
                               " preview=" + compact_excerpt(log.raw_transcript));
                }
            }

            std::string output_text = log.raw_transcript;
            debug_line(std::string("[CORRECTION_ENABLED] ") + (log.correction_enabled ? "true" : "false"));
            debug_line("[CORRECTION_MODE] " + log.correction_mode);

            if (log.correction_enabled && !log.raw_transcript.empty()) {
                diagnostics::diag_begin(session_id_, diagnostics::DiagnosticStage::Correction, "correction begin");
                CorrectionRunInfo info;
                if (correct_transcript_text_with_info(
                        log.raw_transcript, log.formatted_text, log.correction_error, &info) &&
                    !log.formatted_text.empty()) {
                    log.correction_mode = info.correction_mode;
                    log.llm_backend = info.backend_used;
                    log.correction_raw_stdout = info.raw_stdout_excerpt;
                    log.correction_raw_stderr = info.raw_stderr_excerpt;
                    log.correction_sanitizer_reason = info.sanitizer_reason;
                    log.resident_attempted = info.resident_attempted;
                    log.resident_started = info.resident_started;
                    log.resident_startup_error = info.resident_startup_error;
                    log.resident_endpoint_used = info.resident_endpoint_used.empty() ? info.resident_probe_used : info.resident_endpoint_used;
                    log.resident_http_status = info.resident_http_status;
                    log.resident_error = info.resident_error;
                    log.resident_server_exe = info.resident_server_exe;
                    log.resident_phase = info.resident_phase;
                    log.resident_total_budget_ms = info.resident_total_budget_ms;
                    log.resident_remaining_budget_ms = info.resident_remaining_budget_ms;
                    log.resident_attempt_timeout_ms = info.resident_attempt_timeout_ms;
                    log.resident_request_count = info.resident_request_count;
                    log.resident_last_endpoint = info.resident_last_endpoint;
                    log.resident_last_status = info.resident_last_status;
                    log.resident_last_error = info.resident_last_error;
                    log.resident_reset_reason = info.resident_reset_reason;
                    log.resident_gpu_layers = info.resident_gpu_layers;
                    log.resident_ctx_size = info.resident_ctx_size;
                    log.resident_threads = info.resident_threads;
                    log.resident_args_excerpt = info.resident_args_excerpt;
                    log.resident_probe_response_excerpt = info.resident_probe_response_excerpt;
                    log.oneshot_stderr_cuda_hint = info.oneshot_stderr_cuda_hint;
                    output_text = log.formatted_text;
                    log.correction_applied = true;
                    log.correction_segmented = info.segmented;
                    log.correction_segment_count = info.segment_count;
                    if (!info.failed_segment_indices.empty()) {
                        std::ostringstream failed;
                        for (std::size_t i = 0; i < info.failed_segment_indices.size(); ++i) {
                            if (i > 0) {
                                failed << ",";
                            }
                            failed << info.failed_segment_indices[i];
                        }
                        log.correction_failed_segments = failed.str();
                    }
                    debug_line(std::string("[CORRECTION_SEGMENTED] ") + (log.correction_segmented ? "true" : "false"));
                    debug_line("[CORRECTION_SEGMENT_COUNT] " + std::to_string(log.correction_segment_count));
                    debug_line("[CORRECTION_MAX_OUTPUT_TOKENS] " + std::to_string(log.correction_max_output_tokens));
                    debug_line("[LLM_BACKEND] " + (log.llm_backend.empty() ? "none" : log.llm_backend));
                    debug_line(std::string("[LLM_RESIDENT_ATTEMPTED] ") + (log.resident_attempted ? "true" : "false"));
                    debug_line(std::string("[LLM_RESIDENT_STARTED] ") + (log.resident_started ? "true" : "false"));
                    if (!log.resident_endpoint_used.empty()) {
                        debug_line("[LLM_RESIDENT_ENDPOINT] " + log.resident_endpoint_used);
                    }
                    if (log.resident_http_status > 0) {
                        debug_line("[LLM_RESIDENT_HTTP_STATUS] " + std::to_string(log.resident_http_status));
                    }
                    if (!log.resident_error.empty()) {
                        debug_line("[LLM_RESIDENT_ERROR] " + log.resident_error);
                    }
                    if (!log.resident_phase.empty()) {
                        debug_line("[LLM_RESIDENT_PHASE] " + log.resident_phase);
                    }
                    debug_line("[LLM_RESIDENT_BUDGET] total_ms=" + std::to_string(log.resident_total_budget_ms) +
                               " remaining_ms=" + std::to_string(log.resident_remaining_budget_ms) +
                               " attempt_timeout_ms=" + std::to_string(log.resident_attempt_timeout_ms) +
                               " request_count=" + std::to_string(log.resident_request_count));
                    if (!log.resident_last_endpoint.empty()) {
                        debug_line("[LLM_RESIDENT_LAST_ENDPOINT] " + log.resident_last_endpoint);
                    }
                    if (log.resident_last_status > 0) {
                        debug_line("[LLM_RESIDENT_LAST_STATUS] " + std::to_string(log.resident_last_status));
                    }
                    if (!log.resident_last_error.empty()) {
                        debug_line("[LLM_RESIDENT_LAST_ERROR] " + log.resident_last_error);
                    }
                    if (!log.resident_reset_reason.empty()) {
                        debug_line("[LLM_RESIDENT_RESET_REASON] " + log.resident_reset_reason);
                    }
                    debug_line("[LLM_RESIDENT_CONFIG] gpu_layers=" + std::to_string(log.resident_gpu_layers) +
                               " ctx=" + std::to_string(log.resident_ctx_size) +
                               " threads=" + std::to_string(log.resident_threads));
                    if (!log.resident_server_exe.empty()) {
                        debug_line("[LLM_RESIDENT_SERVER_EXE] " + log.resident_server_exe);
                    }
                    if (!log.resident_args_excerpt.empty()) {
                        debug_line("[LLM_RESIDENT_START_ARGS] " + log.resident_args_excerpt);
                    }
                    if (!log.resident_probe_response_excerpt.empty()) {
                        debug_line("[LLM_RESIDENT_PROBE_RESPONSE] " + log.resident_probe_response_excerpt);
                    }
                    if (!log.correction_raw_stdout.empty()) {
                        debug_line("[CORRECTION_RAW_STDOUT] " + log.correction_raw_stdout);
                    }
                    if (!log.correction_raw_stderr.empty()) {
                        debug_line("[CORRECTION_RAW_STDERR] " + log.correction_raw_stderr);
                    }
                    if (!log.correction_sanitizer_reason.empty()) {
                        debug_line("[CORRECTION_SANITIZER_REASON] " + log.correction_sanitizer_reason);
                    }
                    if (!log.correction_failed_segments.empty()) {
                        debug_line("[CORRECTION_FAILED_SEGMENTS] " + log.correction_failed_segments);
                    }
                    debug_line("[FORMATTED_TEXT] " + log.formatted_text);
                    debug_line("[CORRECTION_APPLIED] true");
                    diagnostics::set_correction_applied(session_id_, true);
                    diagnostics::set_correction_debug(session_id_,
                                                      log.llm_backend,
                                                      "",
                                                      log.correction_sanitizer_reason,
                                                      log.correction_raw_stdout,
                                                      log.correction_raw_stderr,
                                                      log.resident_attempted,
                                                      log.resident_started,
                                                      log.resident_startup_error,
                                                      log.resident_endpoint_used,
                                                      log.resident_http_status,
                                                      log.resident_phase,
                                                      log.resident_remaining_budget_ms,
                                                      log.resident_request_count,
                                                      log.resident_last_error);
                    diagnostics::set_segmentation(session_id_, log.correction_segmented, log.correction_segment_count);
                    diagnostics::diag_end(session_id_,
                                          diagnostics::DiagnosticStage::Correction,
                                          "correction applied",
                                          true,
                                          true);
                } else {
                    if (!info.correction_mode.empty()) {
                        log.correction_mode = info.correction_mode;
                    }
                    log.llm_backend = info.backend_used;
                    log.correction_raw_stdout = info.raw_stdout_excerpt;
                    log.correction_raw_stderr = info.raw_stderr_excerpt;
                    log.correction_sanitizer_reason = info.sanitizer_reason;
                    log.resident_attempted = info.resident_attempted;
                    log.resident_started = info.resident_started;
                    log.resident_startup_error = info.resident_startup_error;
                    log.resident_endpoint_used = info.resident_endpoint_used.empty() ? info.resident_probe_used : info.resident_endpoint_used;
                    log.resident_http_status = info.resident_http_status;
                    log.resident_error = info.resident_error.empty() ? info.resident_startup_error : info.resident_error;
                    log.resident_server_exe = info.resident_server_exe;
                    log.resident_phase = info.resident_phase;
                    log.resident_total_budget_ms = info.resident_total_budget_ms;
                    log.resident_remaining_budget_ms = info.resident_remaining_budget_ms;
                    log.resident_attempt_timeout_ms = info.resident_attempt_timeout_ms;
                    log.resident_request_count = info.resident_request_count;
                    log.resident_last_endpoint = info.resident_last_endpoint;
                    log.resident_last_status = info.resident_last_status;
                    log.resident_last_error = info.resident_last_error;
                    log.resident_reset_reason = info.resident_reset_reason;
                    log.resident_gpu_layers = info.resident_gpu_layers;
                    log.resident_ctx_size = info.resident_ctx_size;
                    log.resident_threads = info.resident_threads;
                    log.resident_args_excerpt = info.resident_args_excerpt;
                    log.resident_probe_response_excerpt = info.resident_probe_response_excerpt;
                    log.oneshot_stderr_cuda_hint = info.oneshot_stderr_cuda_hint;
                    log.correction_segmented = info.segmented;
                    log.correction_segment_count = info.segment_count;
                    if (!info.failed_segment_indices.empty()) {
                        std::ostringstream failed;
                        for (std::size_t i = 0; i < info.failed_segment_indices.size(); ++i) {
                            if (i > 0) {
                                failed << ",";
                            }
                            failed << info.failed_segment_indices[i];
                        }
                        log.correction_failed_segments = failed.str();
                    }
                    debug_line("[LLM_BACKEND] " + (log.llm_backend.empty() ? "none" : log.llm_backend));
                    debug_line(std::string("[LLM_RESIDENT_ATTEMPTED] ") + (log.resident_attempted ? "true" : "false"));
                    debug_line(std::string("[LLM_RESIDENT_STARTED] ") + (log.resident_started ? "true" : "false"));
                    if (!log.resident_endpoint_used.empty()) {
                        debug_line("[LLM_RESIDENT_ENDPOINT] " + log.resident_endpoint_used);
                    }
                    if (log.resident_http_status > 0) {
                        debug_line("[LLM_RESIDENT_HTTP_STATUS] " + std::to_string(log.resident_http_status));
                    }
                    if (!log.resident_error.empty()) {
                        debug_line("[LLM_RESIDENT_ERROR] " + log.resident_error);
                    }
                    if (!log.resident_phase.empty()) {
                        debug_line("[LLM_RESIDENT_PHASE] " + log.resident_phase);
                    }
                    debug_line("[LLM_RESIDENT_BUDGET] total_ms=" + std::to_string(log.resident_total_budget_ms) +
                               " remaining_ms=" + std::to_string(log.resident_remaining_budget_ms) +
                               " attempt_timeout_ms=" + std::to_string(log.resident_attempt_timeout_ms) +
                               " request_count=" + std::to_string(log.resident_request_count));
                    if (!log.resident_last_endpoint.empty()) {
                        debug_line("[LLM_RESIDENT_LAST_ENDPOINT] " + log.resident_last_endpoint);
                    }
                    if (log.resident_last_status > 0) {
                        debug_line("[LLM_RESIDENT_LAST_STATUS] " + std::to_string(log.resident_last_status));
                    }
                    if (!log.resident_last_error.empty()) {
                        debug_line("[LLM_RESIDENT_LAST_ERROR] " + log.resident_last_error);
                    }
                    if (!log.resident_reset_reason.empty()) {
                        debug_line("[LLM_RESIDENT_RESET_REASON] " + log.resident_reset_reason);
                    }
                    debug_line("[LLM_RESIDENT_CONFIG] gpu_layers=" + std::to_string(log.resident_gpu_layers) +
                               " ctx=" + std::to_string(log.resident_ctx_size) +
                               " threads=" + std::to_string(log.resident_threads));
                    if (!log.resident_server_exe.empty()) {
                        debug_line("[LLM_RESIDENT_SERVER_EXE] " + log.resident_server_exe);
                    }
                    if (!log.resident_args_excerpt.empty()) {
                        debug_line("[LLM_RESIDENT_START_ARGS] " + log.resident_args_excerpt);
                    }
                    if (!log.resident_probe_response_excerpt.empty()) {
                        debug_line("[LLM_RESIDENT_PROBE_RESPONSE] " + log.resident_probe_response_excerpt);
                    }
                    if (!log.correction_raw_stdout.empty()) {
                        debug_line("[CORRECTION_RAW_STDOUT] " + log.correction_raw_stdout);
                    }
                    if (!log.correction_raw_stderr.empty()) {
                        debug_line("[CORRECTION_RAW_STDERR] " + log.correction_raw_stderr);
                    }
                    if (!log.correction_sanitizer_reason.empty()) {
                        debug_line("[CORRECTION_SANITIZER_REASON] " + log.correction_sanitizer_reason);
                    }
                    debug_line("[CORRECTION_ERROR] " + log.correction_error, true);
                    debug_line("[CORRECTION_FAILED] " + log.correction_error, true);
                    debug_line("[CORRECTION_APPLIED] false");
                    diagnostics::set_correction_applied(session_id_, false);
                    diagnostics::set_correction_debug(session_id_,
                                                      log.llm_backend,
                                                      log.correction_error,
                                                      log.correction_sanitizer_reason,
                                                      log.correction_raw_stdout,
                                                      log.correction_raw_stderr,
                                                      log.resident_attempted,
                                                      log.resident_started,
                                                      log.resident_startup_error,
                                                      log.resident_endpoint_used,
                                                      log.resident_http_status,
                                                      log.resident_phase,
                                                      log.resident_remaining_budget_ms,
                                                      log.resident_request_count,
                                                      log.resident_last_error);
                    diagnostics::set_segmentation(session_id_, log.correction_segmented, log.correction_segment_count);
                    diagnostics::diag_end(session_id_,
                                          diagnostics::DiagnosticStage::Correction,
                                          log.correction_error.empty() ? "correction skipped" : log.correction_error,
                                          true,
                                          false);
                }
            } else if (!log.raw_transcript.empty()) {
                debug_line("[CORRECTION_APPLIED] false");
                diagnostics::set_correction_applied(session_id_, false);
                diagnostics::set_correction_debug(session_id_, "none", "", "", "", "", false, false, "", "", 0, "", 0, 0, "");
                diagnostics::diag_point(session_id_, diagnostics::DiagnosticStage::Correction, "correction not used");
            }

            log.llm_output_chars = static_cast<int>(output_text.size());
            log.llm_output_words = rough_word_count(output_text);
            debug_line("[HANDOFF_WHISPER_TO_LLM_END] backend=" + (log.llm_backend.empty() ? "none" : log.llm_backend) +
                       " chars=" + std::to_string(log.llm_output_chars) + " words=" + std::to_string(log.llm_output_words) +
                       " preview=" + compact_excerpt(output_text));

            diagnostics::diag_begin(session_id_, diagnostics::DiagnosticStage::Sanitization, "sanitize output");
            output_text = sanitize_payload_text(output_text);
            diagnostics::diag_end(session_id_,
                                  diagnostics::DiagnosticStage::Sanitization,
                                  output_text.empty() ? "no output after sanitization" : "sanitized output",
                                  true,
                                  !output_text.empty());

            if (!output_text.empty()) {
                diagnostics::diag_begin(session_id_, diagnostics::DiagnosticStage::Paste, "paste begin");
                HWND current = GetForegroundWindow();
                const bool target_valid = target_window_ && IsWindow(target_window_) && IsWindowVisible(target_window_);
                debug_line("[HANDOFF_LLM_TO_WINDOWS_BEGIN] chars=" + std::to_string(log.llm_output_chars) +
                           " words=" + std::to_string(log.llm_output_words) +
                           " target_valid=" + (target_valid ? "true" : "false") +
                           " focus_match=" + ((current == target_window_) ? "true" : "false") +
                           " target_hwnd=" + hwnd_to_string(target_window_) +
                           " current_hwnd=" + hwnd_to_string(current));

                if (!target_valid) {
                    log.paste_skipped = true;
                    log.paste_error = "Target window is no longer valid/visible.";
                    debug_line("[PASTE_SKIPPED] " + log.paste_error, true);
                    debug_line("[HANDOFF_LLM_TO_WINDOWS_END] outcome=skipped reason=" + log.paste_error, true);
                    diagnostics::set_paste_outcome(session_id_, diagnostics::PasteOutcome::Skipped);
                    diagnostics::diag_end(session_id_, diagnostics::DiagnosticStage::Paste, log.paste_error, true, false);
                } else if (current != target_window_) {
                    log.paste_skipped = true;
                    log.paste_error = "Focus changed; paste skipped to avoid disruptive window changes.";
                    debug_line("[PASTE_SKIPPED] " + log.paste_error, true);
                    debug_line("[HANDOFF_LLM_TO_WINDOWS_END] outcome=skipped reason=" + log.paste_error, true);
                    diagnostics::set_paste_outcome(session_id_, diagnostics::PasteOutcome::Skipped);
                    diagnostics::diag_end(session_id_, diagnostics::DiagnosticStage::Paste, log.paste_error, true, false);
                } else {
                    std::string inject_error;
                    if (!inject_text_via_clipboard_paste(target_window_, output_text, inject_error)) {
                        log.paste_failed = true;
                        log.paste_error = inject_error;
                        debug_line("[PASTE_FAILED] " + inject_error, true);
                        debug_line("[HANDOFF_LLM_TO_WINDOWS_END] outcome=failed reason=" + inject_error, true);
                        diagnostics::set_paste_outcome(session_id_, diagnostics::PasteOutcome::Failed);
                        diagnostics::diag_end(session_id_, diagnostics::DiagnosticStage::Paste, inject_error, true, false);
                    } else {
                        log.paste_done = true;
                        debug_line("[PASTE_DONE]");
                        debug_line("[HANDOFF_LLM_TO_WINDOWS_END] outcome=done");
                        diagnostics::set_paste_outcome(session_id_, diagnostics::PasteOutcome::Done);
                        diagnostics::diag_end(session_id_, diagnostics::DiagnosticStage::Paste, "paste done", true, true);
                    }
                }
            } else if (log.transcribe_error.empty()) {
                log.paste_skipped = true;
                log.paste_error = "No transcript text available for paste.";
                debug_line("[PASTE_SKIPPED] " + log.paste_error);
                diagnostics::set_paste_outcome(session_id_, diagnostics::PasteOutcome::Skipped);
                diagnostics::diag_point(session_id_, diagnostics::DiagnosticStage::Paste, log.paste_error, true, false);
            } else {
                diagnostics::set_paste_outcome(session_id_, diagnostics::PasteOutcome::Skipped);
                diagnostics::diag_point(session_id_,
                                        diagnostics::DiagnosticStage::Paste,
                                        "paste skipped due to transcription failure",
                                        true,
                                        false);
            }

            append_log(log_path, log);
            diagnostics::finish_session(session_id_, "session complete");
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
        out << "[CORRECTION_MODE] " << log.correction_mode << "\n";
        out << "[CORRECTION_SEGMENTED] " << (log.correction_segmented ? "true" : "false") << "\n";
        out << "[CORRECTION_SEGMENT_COUNT] " << log.correction_segment_count << "\n";
        out << "[CORRECTION_MAX_OUTPUT_TOKENS] " << log.correction_max_output_tokens << "\n";
        out << "[LLM_BACKEND] " << (log.llm_backend.empty() ? "none" : log.llm_backend) << "\n";
        if (!log.correction_raw_stdout.empty()) {
            out << "[CORRECTION_RAW_STDOUT] " << log.correction_raw_stdout << "\n";
        }
        if (!log.correction_raw_stderr.empty()) {
            out << "[CORRECTION_RAW_STDERR] " << log.correction_raw_stderr << "\n";
        }
        if (!log.correction_sanitizer_reason.empty()) {
            out << "[CORRECTION_SANITIZER_REASON] " << log.correction_sanitizer_reason << "\n";
        }
        out << "[LLM_RESIDENT_ATTEMPTED] " << (log.resident_attempted ? "true" : "false") << "\n";
        out << "[LLM_RESIDENT_STARTED] " << (log.resident_started ? "true" : "false") << "\n";
        if (!log.resident_startup_error.empty()) {
            out << "[LLM_RESIDENT_STARTUP_ERROR] " << log.resident_startup_error << "\n";
        }
        if (!log.resident_endpoint_used.empty()) {
            out << "[LLM_RESIDENT_ENDPOINT] " << log.resident_endpoint_used << "\n";
        }
        if (log.resident_http_status > 0) {
            out << "[LLM_RESIDENT_HTTP_STATUS] " << log.resident_http_status << "\n";
        }
        if (!log.resident_error.empty()) {
            out << "[LLM_RESIDENT_ERROR] " << log.resident_error << "\n";
        }
        if (!log.resident_phase.empty()) {
            out << "[LLM_RESIDENT_PHASE] " << log.resident_phase << "\n";
        }
        out << "[LLM_RESIDENT_BUDGET] total_ms=" << log.resident_total_budget_ms
            << " remaining_ms=" << log.resident_remaining_budget_ms
            << " attempt_timeout_ms=" << log.resident_attempt_timeout_ms
            << " request_count=" << log.resident_request_count << "\n";
        if (!log.resident_last_endpoint.empty()) {
            out << "[LLM_RESIDENT_LAST_ENDPOINT] " << log.resident_last_endpoint << "\n";
        }
        if (log.resident_last_status > 0) {
            out << "[LLM_RESIDENT_LAST_STATUS] " << log.resident_last_status << "\n";
        }
        if (!log.resident_last_error.empty()) {
            out << "[LLM_RESIDENT_LAST_ERROR] " << log.resident_last_error << "\n";
        }
        if (!log.resident_reset_reason.empty()) {
            out << "[LLM_RESIDENT_RESET_REASON] " << log.resident_reset_reason << "\n";
        }
        out << "[LLM_RESIDENT_CONFIG] gpu_layers=" << log.resident_gpu_layers
            << " ctx=" << log.resident_ctx_size
            << " threads=" << log.resident_threads << "\n";
        if (!log.resident_server_exe.empty()) {
            out << "[LLM_RESIDENT_SERVER_EXE] " << log.resident_server_exe << "\n";
        }
        if (!log.resident_args_excerpt.empty()) {
            out << "[LLM_RESIDENT_START_ARGS] " << log.resident_args_excerpt << "\n";
        }
        if (!log.resident_probe_response_excerpt.empty()) {
            out << "[LLM_RESIDENT_PROBE_RESPONSE] " << log.resident_probe_response_excerpt << "\n";
        }
        out << "[ONESHOT_STDERR_CUDA_HINT] " << (log.oneshot_stderr_cuda_hint ? "true" : "false") << "\n";
        out << "[WHISPER_TEXT_METRICS] chars=" << log.whisper_text_chars << " words=" << log.whisper_text_words << "\n";
        out << "[LLM_OUTPUT_METRICS] chars=" << log.llm_output_chars << " words=" << log.llm_output_words << "\n";

        if (!log.raw_transcript.empty()) {
            out << "[RAW_WHISPER] " << log.raw_transcript << "\n";
        }
        if (!log.formatted_text.empty()) {
            out << "[FORMATTED_TEXT] " << log.formatted_text << "\n";
        }
        out << "[CORRECTION_APPLIED] " << (log.correction_applied ? "true" : "false") << "\n";

        if (!log.correction_failed_segments.empty()) {
            out << "[CORRECTION_FAILED_SEGMENTS] " << log.correction_failed_segments << "\n";
        }
        if (!log.correction_error.empty()) {
            out << "[CORRECTION_ERROR] " << log.correction_error << "\n";
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
        diagnostics::LiveState diag_state = diagnostics::LiveState::Transcribing;
        if (std::wcscmp(state, L"Idle") == 0) {
            state_marker = "Idle";
            diag_state = diagnostics::LiveState::Idle;
        } else if (std::wcscmp(state, L"Recording") == 0) {
            state_marker = "Recording";
            diag_state = diagnostics::LiveState::Recording;
        }
        debug_line("[LIVE_STATE] " + state_marker);
        diagnostics::set_live_state(diag_state);
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
        AppendMenuW(menu, MF_STRING, ID_TRAY_DASHBOARD, L"Dashboard");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void open_dashboard() {
        if (!dashboard::show_dashboard_window(window_, debug_console_)) {
            debug_line("[DASHBOARD_OPEN_FAILED]", true);
        }
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
        dashboard::close_dashboard_window();
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
    uint64_t session_id_ = 0;
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
