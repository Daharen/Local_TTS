#pragma once

#include "audio_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct StreamingDictationResult {
    bool finalized = false;
    bool used_partial_fallback = false;
    int decode_iteration_count = 0;
    int latest_partial_chars = 0;
    std::string latest_partial_text;
    std::string final_text;
    std::string error;
    int64_t stream_first_partial_ms = -1;
    double stream_final_infer_ms = 0.0;
};

class StreamingDictationSession {
public:
    explicit StreamingDictationSession(AudioCapture& capture);
    ~StreamingDictationSession();

    bool start(uint64_t session_id);
    StreamingDictationResult finalize();
    void stop_without_finalize();

private:
    void worker_loop();
    static std::vector<float> tail_copy(const std::vector<float>& input, std::size_t samples);

    AudioCapture& capture_;
    uint64_t session_id_ = 0;
    std::atomic<bool> is_active_{false};
    std::atomic<bool> stop_requested_{false};
    std::chrono::steady_clock::time_point capture_started_at_{};
    std::chrono::steady_clock::time_point last_decode_time_{};

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string latest_partial_text_;
    int decode_iteration_count_ = 0;
    int64_t stream_first_partial_ms_ = -1;
    std::vector<float> keep_tail_pcm_;

    int step_ms_ = 1200;
    int length_ms_ = 6000;
    int keep_ms_ = 250;
    bool finalize_on_release_ = true;
};
