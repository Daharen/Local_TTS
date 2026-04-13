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
    bool segmented = false;
    int segment_count = 0;
    PasteOutcome paste_outcome = PasteOutcome::Unknown;
};

struct DiagnosticsSnapshot {
    LiveState state = LiveState::Idle;
    PipelineSnapshot latest_session;
    std::vector<DiagnosticEvent> recent_events;
};

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

}  // namespace diagnostics
