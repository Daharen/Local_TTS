#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace diagnostics {
namespace {

#ifdef _WIN32
constexpr UINT kDashboardUpdateMessage = WM_APP + 140;
#else
constexpr UINT kDashboardUpdateMessage = 140;
#endif
constexpr std::size_t kMaxEvents = 600;
constexpr std::size_t kMaxSessions = 64;

struct SessionWork {
    PipelineSnapshot snapshot;
    std::array<std::chrono::steady_clock::time_point, 8> begin_points{};
    std::array<bool, 8> begin_valid{};
    std::chrono::steady_clock::time_point hotkey_time{};
    bool hotkey_valid = false;
    std::chrono::steady_clock::time_point recording_stop_time{};
    bool recording_stop_valid = false;
};

int stage_index(DiagnosticStage stage) {
    switch (stage) {
        case DiagnosticStage::WavWrite:
            return 0;
        case DiagnosticStage::Whisper:
            return 1;
        case DiagnosticStage::Correction:
            return 2;
        case DiagnosticStage::Sanitization:
            return 3;
        case DiagnosticStage::Paste:
            return 4;
        case DiagnosticStage::RecordingStart:
            return 5;
        case DiagnosticStage::RecordingStop:
            return 6;
        case DiagnosticStage::HotkeyDetected:
            return 7;
        case DiagnosticStage::SessionComplete:
        default:
            return -1;
    }
}

class Store {
public:
    uint64_t begin_session() noexcept {
        try {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mu_);
            const uint64_t id = ++last_session_id_;
            SessionWork work;
            work.snapshot.session_id = id;
            work.snapshot.started_ms = since_start_ms(now);
            work.hotkey_time = now;
            work.hotkey_valid = true;
            sessions_[id] = work;
            trim_sessions_locked();
            append_event_locked(id, now, DiagnosticStage::HotkeyDetected, DiagnosticEventKind::Point, -1,
                                "Hotkey detected", false, false);
            return id;
        } catch (...) {
            return 0;
        }
    }

    void finish_session(uint64_t session_id, const std::string& message) noexcept {
        add_event(session_id, DiagnosticStage::SessionComplete, DiagnosticEventKind::Point, message, false, false, -1);
        try {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                latest_session_ = it->second.snapshot;
                sessions_.erase(it);
            }
        } catch (...) {
        }
    }

    void set_live_state(LiveState state) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            state_ = state;
        } catch (...) {
        }
        notify_dashboard();
    }

    void add_event(uint64_t session_id,
                   DiagnosticStage stage,
                   DiagnosticEventKind kind,
                   const std::string& message,
                   bool has_success,
                   bool success,
                   int64_t forced_duration_ms) noexcept {
        try {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mu_);
            auto& session = sessions_[session_id];
            if (session.snapshot.session_id == 0) {
                session.snapshot.session_id = session_id;
                session.snapshot.started_ms = since_start_ms(now);
                session.hotkey_time = now;
                session.hotkey_valid = true;
            }

            int64_t duration_ms = forced_duration_ms;
            const int idx = stage_index(stage);
            if (kind == DiagnosticEventKind::Begin && idx >= 0) {
                session.begin_points[static_cast<std::size_t>(idx)] = now;
                session.begin_valid[static_cast<std::size_t>(idx)] = true;
            } else if (kind == DiagnosticEventKind::End && idx >= 0 && session.begin_valid[static_cast<std::size_t>(idx)]) {
                duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - session.begin_points[static_cast<std::size_t>(idx)])
                                  .count();
                session.begin_valid[static_cast<std::size_t>(idx)] = false;
                apply_duration(session.snapshot, stage, duration_ms);
            }

            if (stage == DiagnosticStage::RecordingStop && kind == DiagnosticEventKind::Point) {
                session.recording_stop_time = now;
                session.recording_stop_valid = true;
                if (session.hotkey_valid) {
                    session.snapshot.recording_duration_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - session.hotkey_time).count();
                }
            }

            if (stage == DiagnosticStage::Paste && kind == DiagnosticEventKind::End) {
                if (session.recording_stop_valid) {
                    session.snapshot.from_recording_stop_to_paste_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - session.recording_stop_time).count();
                }
                if (session.hotkey_valid) {
                    session.snapshot.total_end_to_end_duration_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - session.hotkey_time).count();
                }
            }

            append_event_locked(session_id, now, stage, kind, duration_ms, message, has_success, success);
        } catch (...) {
        }
        notify_dashboard();
    }

    void set_correction_applied(uint64_t session_id, bool applied) noexcept {
        update_session(session_id, [applied](SessionWork& s) { s.snapshot.correction_applied = applied; });
    }

    void set_correction_debug(uint64_t session_id,
                              const std::string& backend,
                              const std::string& error,
                              const std::string& sanitizer_reason,
                              const std::string& raw_stdout_excerpt,
                              const std::string& raw_stderr_excerpt) noexcept {
        update_session(session_id, [&](SessionWork& s) {
            s.snapshot.correction_backend = backend;
            s.snapshot.correction_error = error;
            s.snapshot.correction_sanitizer_reason = sanitizer_reason;
            s.snapshot.correction_raw_stdout_excerpt = raw_stdout_excerpt;
            s.snapshot.correction_raw_stderr_excerpt = raw_stderr_excerpt;
        });
    }

    void set_segmentation(uint64_t session_id, bool segmented, int segment_count) noexcept {
        update_session(session_id, [segmented, segment_count](SessionWork& s) {
            s.snapshot.segmented = segmented;
            s.snapshot.segment_count = segment_count;
        });
    }

    void set_paste_outcome(uint64_t session_id, PasteOutcome outcome) noexcept {
        update_session(session_id, [outcome](SessionWork& s) { s.snapshot.paste_outcome = outcome; });
    }

    void set_recording_stop_time(uint64_t session_id) noexcept {
        try {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mu_);
            auto it = sessions_.find(session_id);
            if (it == sessions_.end()) {
                return;
            }
            it->second.recording_stop_time = now;
            it->second.recording_stop_valid = true;
        } catch (...) {
        }
    }

    DiagnosticsSnapshot snapshot(std::size_t max_events) noexcept {
        DiagnosticsSnapshot out;
        try {
            std::lock_guard<std::mutex> lock(mu_);
            out.state = state_;
            out.latest_session = latest_session_;
            const std::size_t take = (std::min)(max_events, events_.size());
            out.recent_events.reserve(take);
            auto start = events_.end() - static_cast<std::ptrdiff_t>(take);
            out.recent_events.insert(out.recent_events.end(), start, events_.end());
        } catch (...) {
        }
        return out;
    }

    void register_dashboard_window(HWND hwnd) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            dashboard_hwnd_ = hwnd;
        } catch (...) {
        }
    }

    void unregister_dashboard_window(HWND hwnd) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (dashboard_hwnd_ == hwnd) {
                dashboard_hwnd_ = nullptr;
            }
        } catch (...) {
        }
    }

private:
    void trim_sessions_locked() {
        while (sessions_.size() > kMaxSessions) {
            auto oldest = sessions_.begin();
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                if (it->first < oldest->first) {
                    oldest = it;
                }
            }
            sessions_.erase(oldest);
        }
    }

    template <typename Fn>
    void update_session(uint64_t session_id, Fn fn) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = sessions_.find(session_id);
            if (it == sessions_.end()) {
                return;
            }
            fn(it->second);
        } catch (...) {
        }
        notify_dashboard();
    }

    void append_event_locked(uint64_t session_id,
                             const std::chrono::steady_clock::time_point& now,
                             DiagnosticStage stage,
                             DiagnosticEventKind kind,
                             int64_t duration_ms,
                             const std::string& message,
                             bool has_success,
                             bool success) {
        DiagnosticEvent e;
        e.sequence = ++last_sequence_;
        e.session_id = session_id;
        e.timestamp_ms = since_start_ms(now);
        e.stage = stage;
        e.kind = kind;
        e.duration_ms = duration_ms;
        e.has_success = has_success;
        e.success = success;
        e.message = message;
        events_.push_back(std::move(e));
        while (events_.size() > kMaxEvents) {
            events_.pop_front();
        }
    }

    static void apply_duration(PipelineSnapshot& snap, DiagnosticStage stage, int64_t duration_ms) {
        switch (stage) {
            case DiagnosticStage::WavWrite:
                snap.wav_write_duration_ms = duration_ms;
                break;
            case DiagnosticStage::Whisper:
                snap.whisper_duration_ms = duration_ms;
                break;
            case DiagnosticStage::Correction:
                snap.correction_duration_ms = duration_ms;
                break;
            case DiagnosticStage::Sanitization:
                snap.sanitization_duration_ms = duration_ms;
                break;
            case DiagnosticStage::Paste:
                snap.paste_duration_ms = duration_ms;
                break;
            default:
                break;
        }
    }

    int64_t since_start_ms(const std::chrono::steady_clock::time_point& tp) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(tp - started_).count();
    }

    void notify_dashboard() noexcept {
#ifdef _WIN32
        HWND hwnd = nullptr;
        {
            try {
                std::lock_guard<std::mutex> lock(mu_);
                hwnd = dashboard_hwnd_;
            } catch (...) {
                hwnd = nullptr;
            }
        }
        if (hwnd) {
            PostMessageW(hwnd, kDashboardUpdateMessage, 0, 0);
        }
#endif
    }

    std::mutex mu_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::deque<DiagnosticEvent> events_;
    std::unordered_map<uint64_t, SessionWork> sessions_;
    PipelineSnapshot latest_session_{};
    uint64_t last_session_id_ = 0;
    uint64_t last_sequence_ = 0;
    LiveState state_ = LiveState::Idle;
    HWND dashboard_hwnd_ = nullptr;
};

Store& store() {
    static Store s;
    return s;
}

}  // namespace

UINT dashboard_update_message() noexcept {
    return kDashboardUpdateMessage;
}

void set_live_state(LiveState state) noexcept {
    store().set_live_state(state);
}

uint64_t begin_session() noexcept {
    return store().begin_session();
}

void finish_session(uint64_t session_id, const std::string& message) noexcept {
    store().finish_session(session_id, message);
}

void diag_point(uint64_t session_id,
                DiagnosticStage stage,
                const std::string& message,
                bool has_success,
                bool success) noexcept {
    store().add_event(session_id, stage, DiagnosticEventKind::Point, message, has_success, success, -1);
}

void diag_begin(uint64_t session_id, DiagnosticStage stage, const std::string& message) noexcept {
    store().add_event(session_id, stage, DiagnosticEventKind::Begin, message, false, false, -1);
}

void diag_end(uint64_t session_id,
              DiagnosticStage stage,
              const std::string& message,
              bool has_success,
              bool success) noexcept {
    store().add_event(session_id, stage, DiagnosticEventKind::End, message, has_success, success, -1);
}

void set_correction_applied(uint64_t session_id, bool applied) noexcept {
    store().set_correction_applied(session_id, applied);
}

void set_correction_debug(uint64_t session_id,
                          const std::string& backend,
                          const std::string& error,
                          const std::string& sanitizer_reason,
                          const std::string& raw_stdout_excerpt,
                          const std::string& raw_stderr_excerpt) noexcept {
    store().set_correction_debug(session_id, backend, error, sanitizer_reason, raw_stdout_excerpt, raw_stderr_excerpt);
}

void set_segmentation(uint64_t session_id, bool segmented, int segment_count) noexcept {
    store().set_segmentation(session_id, segmented, segment_count);
}

void set_paste_outcome(uint64_t session_id, PasteOutcome outcome) noexcept {
    store().set_paste_outcome(session_id, outcome);
}

void set_recording_stop_time(uint64_t session_id) noexcept {
    store().set_recording_stop_time(session_id);
}

DiagnosticsSnapshot get_snapshot(std::size_t max_events) noexcept {
    return store().snapshot(max_events);
}

void register_dashboard_window(HWND hwnd) noexcept {
    store().register_dashboard_window(hwnd);
}

void unregister_dashboard_window(HWND hwnd) noexcept {
    store().unregister_dashboard_window(hwnd);
}

const char* stage_name(DiagnosticStage stage) noexcept {
    switch (stage) {
        case DiagnosticStage::HotkeyDetected:
            return "hotkey";
        case DiagnosticStage::RecordingStart:
            return "record_start";
        case DiagnosticStage::RecordingStop:
            return "record_stop";
        case DiagnosticStage::WavWrite:
            return "wav_write";
        case DiagnosticStage::Whisper:
            return "whisper";
        case DiagnosticStage::Correction:
            return "correction";
        case DiagnosticStage::Sanitization:
            return "sanitize";
        case DiagnosticStage::Paste:
            return "paste";
        case DiagnosticStage::SessionComplete:
            return "session_complete";
        default:
            return "unknown";
    }
}

const char* event_kind_name(DiagnosticEventKind kind) noexcept {
    switch (kind) {
        case DiagnosticEventKind::Begin:
            return "begin";
        case DiagnosticEventKind::End:
            return "end";
        case DiagnosticEventKind::Point:
            return "point";
        default:
            return "unknown";
    }
}

const char* live_state_name(LiveState state) noexcept {
    switch (state) {
        case LiveState::Idle:
            return "Idle";
        case LiveState::Recording:
            return "Recording";
        case LiveState::Transcribing:
            return "Transcribing";
        default:
            return "Idle";
    }
}

const char* paste_outcome_name(PasteOutcome outcome) noexcept {
    switch (outcome) {
        case PasteOutcome::Done:
            return "done";
        case PasteOutcome::Skipped:
            return "skipped";
        case PasteOutcome::Failed:
            return "failed";
        case PasteOutcome::Unknown:
        default:
            return "unknown";
    }
}

}  // namespace diagnostics
