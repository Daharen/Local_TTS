#pragma once

#include "audio_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct StreamingSegment {
    std::string text;
    int t0_ms = 0;
    int t1_ms = 0;
    std::vector<int> token_ids;
};

struct StreamingHypothesis {
    uint64_t decode_index = 0;
    std::string full_text;
    std::vector<std::string> tokens_original;
    std::vector<std::string> tokens_normalized;
    std::vector<int> whisper_prompt_tokens;
    std::vector<StreamingSegment> segments;
    int audio_buffer_start_ms = 0;
    int audio_buffer_end_ms = 0;
};

struct StreamingDictationResult {
    bool finalized = false;
    bool waited_for_inflight_decode = false;
    int decode_iteration_count = 0;
    int latest_partial_chars = 0;
    int committed_chars = 0;
    int committed_prefix_chars = 0;
    int final_candidate_chars = 0;
    int stream_agreement_n = 0;
    int stream_history_depth = 0;
    int stream_hypothesis_chars = 0;
    int stream_confirmed_prefix_tokens = 0;
    int stream_commit_advance_tokens = 0;
    int stream_unconfirmed_suffix_chars = 0;
    int stream_prompt_tokens = 0;
    int stream_audio_trim_ms = 0;
    int stream_buffer_base_ms = 0;
    int stream_local_agreement_match_tokens = 0;
    bool stream_segment_trim_used = false;
    std::string latest_partial_text;
    std::string committed_text;
    std::string committed_prefix_text;
    std::string final_candidate_text;
    std::string final_text;
    std::string error;
    std::string stream_finalization_source = "local_agreement";
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

    AudioCapture& capture_;
    uint64_t session_id_ = 0;
    std::atomic<bool> is_active_{false};
    std::atomic<bool> stop_requested_{false};
    std::chrono::steady_clock::time_point capture_started_at_{};

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::deque<StreamingHypothesis> hypothesis_history_;
    std::string committed_prefix_text_;
    std::vector<std::string> committed_prefix_tokens_original_;
    std::vector<std::string> committed_prefix_tokens_normalized_;
    std::vector<int> committed_prompt_tokens_;
    std::string latest_unconfirmed_text_;
    std::string final_candidate_text_;
    int committed_audio_ms_ = 0;
    int audio_buffer_base_ms_ = 0;
    std::size_t capture_start_total_samples_ = 0;
    std::size_t audio_buffer_base_samples_ = 0;
    int agreement_failure_count_ = 0;
    int local_agreement_match_tokens_ = 0;
    int commit_advance_tokens_ = 0;
    int latest_audio_trim_ms_ = 0;
    bool latest_segment_trim_used_ = false;
    bool decode_in_progress_ = false;
    int decode_iteration_count_ = 0;
    int64_t stream_first_partial_ms_ = -1;

    int step_ms_ = 1200;
    int length_ms_ = 6000;
    int keep_ms_ = 250;
    bool finalize_on_release_ = true;
    int local_agreement_n_ = 2;
    int prompt_max_tokens_ = 256;
    int trim_guard_ms_ = 250;
    bool trim_on_segment_boundary_ = true;
};
