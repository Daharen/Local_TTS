#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
using HWND = void*;
using UINT = unsigned int;
#endif

namespace diagnostics {

enum class LiveState {
    Idle,
    Recording,
    Transcribing,
};

enum class DiagnosticStage {
    HotkeyDetected,
    RecordingStart,
    RecordingStop,
    WavWrite,
    Whisper,
    Correction,
    Sanitization,
    Paste,
    SessionComplete,
};

enum class DiagnosticEventKind {
    Begin,
    End,
    Point,
};

struct DiagnosticEvent {
    uint64_t sequence = 0;
    uint64_t session_id = 0;
    int64_t timestamp_ms = 0;
    DiagnosticStage stage = DiagnosticStage::SessionComplete;
    DiagnosticEventKind kind = DiagnosticEventKind::Point;
    int64_t duration_ms = -1;
    bool has_success = false;
    bool success = false;
    std::string message;
};

struct TimingSpan {
    DiagnosticStage stage = DiagnosticStage::SessionComplete;
    int64_t duration_ms = -1;
};

enum class PasteOutcome {
    Unknown,
    Done,
    Skipped,
    Failed,
};

struct PipelineSnapshot {
    uint64_t session_id = 0;
    int64_t started_ms = 0;
    int64_t recording_duration_ms = -1;
    int64_t wav_write_duration_ms = -1;
    int64_t whisper_duration_ms = -1;
    int64_t correction_duration_ms = -1;
    int64_t sanitization_duration_ms = -1;
    int64_t paste_duration_ms = -1;
    int64_t from_recording_stop_to_paste_ms = -1;
    int64_t total_end_to_end_duration_ms = -1;
    bool correction_applied = false;
    std::string correction_backend;
    std::string correction_error;
    std::string correction_sanitizer_reason;
    std::string correction_raw_stdout_excerpt;
    std::string correction_raw_stderr_excerpt;
    bool resident_attempted = false;
    bool resident_started = false;
    std::string resident_startup_error;
    std::string resident_endpoint_used;
    int resident_http_status = 0;
    std::string resident_phase;
    int resident_remaining_budget_ms = 0;
    int resident_request_count = 0;
    std::string resident_last_endpoint;
    int resident_last_status = 0;
    std::string resident_last_error;
    std::string resident_reset_reason;
    bool resident_fallback_used = false;
    bool segmented = false;
    int segment_count = 0;
    PasteOutcome paste_outcome = PasteOutcome::Unknown;
};

struct DiagnosticsSnapshot {
    LiveState state = LiveState::Idle;
    PipelineSnapshot latest_session;
    std::vector<DiagnosticEvent> recent_events;
};

#ifndef LOCAL_TTS_ENABLE_DASHBOARD
#define LOCAL_TTS_ENABLE_DASHBOARD 1
#endif

#if LOCAL_TTS_ENABLE_DASHBOARD

UINT dashboard_update_message() noexcept;

void set_live_state(LiveState state) noexcept;
uint64_t begin_session() noexcept;
void finish_session(uint64_t session_id, const std::string& message = std::string()) noexcept;

void diag_point(uint64_t session_id,
                DiagnosticStage stage,
                const std::string& message = std::string(),
                bool has_success = false,
                bool success = false) noexcept;

void diag_begin(uint64_t session_id, DiagnosticStage stage, const std::string& message = std::string()) noexcept;

void diag_end(uint64_t session_id,
              DiagnosticStage stage,
              const std::string& message = std::string(),
              bool has_success = false,
              bool success = false) noexcept;

void set_correction_applied(uint64_t session_id, bool applied) noexcept;
void set_correction_debug(uint64_t session_id,
                          const std::string& backend,
                          const std::string& error,
                          const std::string& sanitizer_reason,
                          const std::string& raw_stdout_excerpt,
                          const std::string& raw_stderr_excerpt,
                          bool resident_attempted,
                          bool resident_started,
                          const std::string& resident_startup_error,
                          const std::string& resident_endpoint_used,
                          int resident_http_status,
                          const std::string& resident_phase,
                          int resident_remaining_budget_ms,
                          int resident_request_count,
                          const std::string& resident_last_endpoint,
                          int resident_last_status,
                          const std::string& resident_last_error,
                          const std::string& resident_reset_reason,
                          bool resident_fallback_used) noexcept;
void set_segmentation(uint64_t session_id, bool segmented, int segment_count) noexcept;
void set_paste_outcome(uint64_t session_id, PasteOutcome outcome) noexcept;
void set_recording_stop_time(uint64_t session_id) noexcept;

DiagnosticsSnapshot get_snapshot(std::size_t max_events = 300) noexcept;

void register_dashboard_window(HWND hwnd) noexcept;
void unregister_dashboard_window(HWND hwnd) noexcept;

const char* stage_name(DiagnosticStage stage) noexcept;
const char* event_kind_name(DiagnosticEventKind kind) noexcept;
const char* live_state_name(LiveState state) noexcept;
const char* paste_outcome_name(PasteOutcome outcome) noexcept;

#else

inline UINT dashboard_update_message() noexcept { return 0; }
inline void set_live_state(LiveState) noexcept {}
inline uint64_t begin_session() noexcept { return 0; }
inline void finish_session(uint64_t, const std::string& = std::string()) noexcept {}
inline void diag_point(uint64_t, DiagnosticStage, const std::string& = std::string(), bool = false, bool = false) noexcept {}
inline void diag_begin(uint64_t, DiagnosticStage, const std::string& = std::string()) noexcept {}
inline void diag_end(uint64_t, DiagnosticStage, const std::string& = std::string(), bool = false, bool = false) noexcept {}
inline void set_correction_applied(uint64_t, bool) noexcept {}
inline void set_correction_debug(uint64_t,
                                 const std::string&,
                                 const std::string&,
                                 const std::string&,
                                 const std::string&,
                                 const std::string&,
                                 bool,
                                 bool,
                                 const std::string&,
                                 const std::string&,
                                 int,
                                 const std::string&,
                                 int,
                                 int,
                                 const std::string&,
                                 int,
                                 const std::string&,
                                 const std::string&,
                                 bool) noexcept {}
inline void set_segmentation(uint64_t, bool, int) noexcept {}
inline void set_paste_outcome(uint64_t, PasteOutcome) noexcept {}
inline void set_recording_stop_time(uint64_t) noexcept {}
inline DiagnosticsSnapshot get_snapshot(std::size_t = 300) noexcept { return DiagnosticsSnapshot{}; }
inline void register_dashboard_window(HWND) noexcept {}
inline void unregister_dashboard_window(HWND) noexcept {}
inline const char* stage_name(DiagnosticStage) noexcept { return "disabled"; }
inline const char* event_kind_name(DiagnosticEventKind) noexcept { return "disabled"; }
inline const char* live_state_name(LiveState) noexcept { return "disabled"; }
inline const char* paste_outcome_name(PasteOutcome) noexcept { return "disabled"; }

#endif

}  // namespace diagnostics
