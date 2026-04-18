#include "streaming_dictation.h"

#include "paths.h"
#include "pipeline_debug.h"
#include "whisper_runner.h"

#include <algorithm>
#include <sstream>

namespace {

using Clock = std::chrono::steady_clock;

std::string build_stream_info_line(const char* tag, int iteration, std::size_t samples) {
    std::ostringstream out;
    out << tag << " iter=" << iteration << " samples=" << samples;
    return out.str();
}

}  // namespace

StreamingDictationSession::StreamingDictationSession(AudioCapture& capture) : capture_(capture) {}

StreamingDictationSession::~StreamingDictationSession() {
    stop_without_finalize();
}

bool StreamingDictationSession::start(uint64_t session_id) {
    if (is_active_.exchange(true)) {
        return false;
    }

    step_ms_ = (std::max)(200, get_stream_step_ms());
    length_ms_ = (std::max)(1000, get_stream_length_ms());
    keep_ms_ = (std::max)(0, get_stream_keep_ms());
    finalize_on_release_ = is_stream_finalize_on_release_enabled();

    session_id_ = session_id;
    stop_requested_.store(false);
    capture_started_at_ = Clock::now();
    last_decode_time_ = capture_started_at_;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_partial_text_.clear();
        decode_iteration_count_ = 0;
        stream_first_partial_ms_ = -1;
        keep_tail_pcm_.clear();
    }

    pipeline_debug::log("streaming", "[STREAM_BEGIN] session_id=" + std::to_string(session_id_) +
                                         " stream_step_ms=" + std::to_string(step_ms_) +
                                         " stream_length_ms=" + std::to_string(length_ms_) +
                                         " stream_keep_ms=" + std::to_string(keep_ms_));

    worker_ = std::thread(&StreamingDictationSession::worker_loop, this);
    return true;
}

void StreamingDictationSession::worker_loop() {
    auto next_due = Clock::now() + std::chrono::milliseconds(step_ms_);
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, next_due, [this]() { return stop_requested_.load(); });
        }
        if (stop_requested_.load()) {
            break;
        }
        next_due += std::chrono::milliseconds(step_ms_);

        std::vector<float> window_pcm;
        capture_.get_latest_pcm_ms(length_ms_, window_pcm);
        if (window_pcm.empty()) {
            continue;
        }

        std::vector<float> decode_pcm;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            decode_pcm = keep_tail_pcm_;
        }
        decode_pcm.insert(decode_pcm.end(), window_pcm.begin(), window_pcm.end());

        int iteration = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++decode_iteration_count_;
            iteration = decode_iteration_count_;
            keep_tail_pcm_ = tail_copy(window_pcm, static_cast<std::size_t>((keep_ms_ * AudioCapture::kSampleRate) / 1000));
        }

        pipeline_debug::log("streaming", build_stream_info_line("[STREAM_DECODE_BEGIN]", iteration, decode_pcm.size()));

        std::string partial;
        std::string error;
        WhisperRunInfo info;
        const bool ok = transcribe_pcm_to_string_with_info(
            decode_pcm.data(), static_cast<int>(decode_pcm.size()), partial, error, &info);

        last_decode_time_ = Clock::now();
        if (ok && !partial.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_partial_text_ = partial;
            if (stream_first_partial_ms_ < 0) {
                stream_first_partial_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(last_decode_time_ - capture_started_at_).count();
            }
            pipeline_debug::log("streaming", "[STREAM_PARTIAL_TEXT] chars=" + std::to_string(latest_partial_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_DECODE_END] iter=" + std::to_string(iteration) + " status=ok infer_ms=" + std::to_string(info.infer_ms));
        } else {
            pipeline_debug::log("streaming", "[STREAM_DECODE_END] iter=" + std::to_string(iteration) + " status=fail error=" + error, true);
        }
    }
}

StreamingDictationResult StreamingDictationSession::finalize() {
    StreamingDictationResult result;
    if (!is_active_.exchange(false)) {
        result.error = "streaming session was not active";
        return result;
    }

    pipeline_debug::log("streaming", "[STREAM_FINALIZE_BEGIN]");
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    std::vector<float> window_pcm;
    capture_.get_latest_pcm_ms(length_ms_, window_pcm);

    std::vector<float> decode_pcm;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decode_pcm = keep_tail_pcm_;
        result.latest_partial_text = latest_partial_text_;
        result.decode_iteration_count = decode_iteration_count_;
        result.stream_first_partial_ms = stream_first_partial_ms_;
    }
    decode_pcm.insert(decode_pcm.end(), window_pcm.begin(), window_pcm.end());

    if (!finalize_on_release_) {
        result.finalized = !result.latest_partial_text.empty();
        result.final_text = result.latest_partial_text;
        result.latest_partial_chars = static_cast<int>(result.latest_partial_text.size());
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=partial_only chars=" + std::to_string(result.latest_partial_chars));
        return result;
    }

    if (decode_pcm.empty()) {
        result.error = "No captured audio available for streaming finalization.";
        result.finalized = false;
        result.latest_partial_chars = static_cast<int>(result.latest_partial_text.size());
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=fail reason=no_audio", true);
        return result;
    }

    std::string text;
    std::string error;
    WhisperRunInfo info;
    const bool ok = transcribe_pcm_to_string_with_info(
        decode_pcm.data(), static_cast<int>(decode_pcm.size()), text, error, &info);

    result.latest_partial_chars = static_cast<int>(result.latest_partial_text.size());
    result.stream_final_infer_ms = info.infer_ms;

    if (ok && !text.empty()) {
        result.finalized = true;
        result.final_text = text;
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=ok chars=" + std::to_string(result.final_text.size()) +
                                             " infer_ms=" + std::to_string(result.stream_final_infer_ms));
        return result;
    }

    if (!result.latest_partial_text.empty()) {
        result.finalized = true;
        result.used_partial_fallback = true;
        result.final_text = result.latest_partial_text;
        result.error = "Final streaming pass failed; falling back to latest partial. " + error;
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=partial_fallback error=" + error, true);
        return result;
    }

    result.error = error.empty() ? "Final streaming pass failed without partial fallback." : error;
    result.finalized = false;
    pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=fail error=" + result.error, true);
    return result;
}

void StreamingDictationSession::stop_without_finalize() {
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    is_active_.store(false);
}

std::vector<float> StreamingDictationSession::tail_copy(const std::vector<float>& input, std::size_t samples) {
    if (samples == 0 || input.empty()) {
        return {};
    }
    const std::size_t take = (std::min)(samples, input.size());
    return std::vector<float>(input.end() - static_cast<std::ptrdiff_t>(take), input.end());
}
