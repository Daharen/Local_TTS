#include "streaming_dictation.h"

#include "paths.h"
#include "pipeline_debug.h"
#include "whisper_runner.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::string build_stream_info_line(const char* tag, int iteration, std::size_t samples) {
    std::ostringstream out;
    out << tag << " iter=" << iteration << " samples=" << samples;
    return out.str();
}

struct TokenData {
    std::string original;
    std::string normalized;
};

std::string collapse_whitespace(std::string text) {
    std::string out;
    out.reserve(text.size());
    bool seen_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            seen_space = true;
            continue;
        }
        if (seen_space && !out.empty()) {
            out.push_back(' ');
        }
        out.push_back(static_cast<char>(ch));
        seen_space = false;
    }
    return out;
}

std::string normalize_token_for_match(const std::string& token) {
    std::size_t start = 0;
    std::size_t end = token.size();
    while (start < end && !std::isalnum(static_cast<unsigned char>(token[start]))) {
        ++start;
    }
    while (end > start && !std::isalnum(static_cast<unsigned char>(token[end - 1]))) {
        --end;
    }
    std::string out;
    out.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(token[i]))));
    }
    return out;
}

std::vector<TokenData> tokenize_for_merge(const std::string& text) {
    std::vector<TokenData> tokens;
    std::istringstream in(text);
    std::string token;
    while (in >> token) {
        TokenData data;
        data.original = token;
        data.normalized = normalize_token_for_match(token);
        if (!data.normalized.empty()) {
            tokens.push_back(std::move(data));
        }
    }
    return tokens;
}

std::string join_token_range(const std::vector<TokenData>& tokens, std::size_t start) {
    std::ostringstream out;
    for (std::size_t i = start; i < tokens.size(); ++i) {
        if (i > start) {
            out << ' ';
        }
        out << tokens[i].original;
    }
    return out.str();
}

bool is_prefix_match(const std::vector<TokenData>& prefix, const std::vector<TokenData>& full) {
    if (prefix.size() > full.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i].normalized != full[i].normalized) {
            return false;
        }
    }
    return true;
}

bool contained_near_end(const std::vector<TokenData>& haystack, const std::vector<TokenData>& needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    const std::size_t start_limit = haystack.size() - needle.size();
    const std::size_t near_end_start = (start_limit > 6) ? (start_limit - 6) : 0;
    for (std::size_t start = near_end_start; start <= start_limit; ++start) {
        bool matches = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            if (haystack[start + i].normalized != needle[i].normalized) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

std::string merge_partial_into_committed(const std::string& committed, const std::string& partial) {
    const std::string committed_clean = collapse_whitespace(committed);
    const std::string partial_clean = collapse_whitespace(partial);
    if (committed_clean.empty()) {
        return partial_clean;
    }
    if (partial_clean.empty()) {
        return committed_clean;
    }

    const auto committed_tokens = tokenize_for_merge(committed_clean);
    const auto partial_tokens = tokenize_for_merge(partial_clean);
    if (committed_tokens.empty()) {
        return partial_clean;
    }
    if (partial_tokens.empty()) {
        return committed_clean;
    }

    if (is_prefix_match(committed_tokens, partial_tokens)) {
        return partial_clean;
    }
    if (contained_near_end(committed_tokens, partial_tokens)) {
        return committed_clean;
    }

    const std::size_t max_overlap = (std::min)(committed_tokens.size(), partial_tokens.size());
    std::size_t best_overlap = 0;
    for (std::size_t k = max_overlap; k > 0; --k) {
        bool overlap = true;
        for (std::size_t i = 0; i < k; ++i) {
            if (committed_tokens[committed_tokens.size() - k + i].normalized != partial_tokens[i].normalized) {
                overlap = false;
                break;
            }
        }
        if (overlap) {
            best_overlap = k;
            break;
        }
    }

    if (best_overlap > 0) {
        if (best_overlap >= partial_tokens.size()) {
            return committed_clean;
        }
        std::string merged = committed_clean;
        merged.push_back(' ');
        merged += join_token_range(partial_tokens, best_overlap);
        return collapse_whitespace(merged);
    }

    std::string merged = committed_clean;
    merged.push_back(' ');
    merged += partial_clean;
    return collapse_whitespace(merged);
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
        committed_text_.clear();
        latest_partial_text_.clear();
        finalized_text_candidate_.clear();
        previous_partial_text_.clear();
        decode_in_progress_ = false;
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
            decode_in_progress_ = true;
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
            previous_partial_text_ = latest_partial_text_;
            latest_partial_text_ = partial;
            committed_text_ = merge_partial_into_committed(committed_text_, partial);
            finalized_text_candidate_ = committed_text_.empty() ? latest_partial_text_ : committed_text_;
            decode_in_progress_ = false;
            if (stream_first_partial_ms_ < 0) {
                stream_first_partial_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(last_decode_time_ - capture_started_at_).count();
            }
            pipeline_debug::log("streaming", "[STREAM_PARTIAL_TEXT] chars=" + std::to_string(latest_partial_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_AGGREGATE_TEXT] chars=" + std::to_string(committed_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_DECODE_END] iter=" + std::to_string(iteration) + " status=ok infer_ms=" + std::to_string(info.infer_ms));
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            decode_in_progress_ = false;
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
    bool waited_for_inflight_decode = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waited_for_inflight_decode = decode_in_progress_;
    }
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.waited_for_inflight_decode = waited_for_inflight_decode;
        result.committed_text = committed_text_;
        result.latest_partial_text = latest_partial_text_;
        result.decode_iteration_count = decode_iteration_count_;
        result.stream_first_partial_ms = stream_first_partial_ms_;
        result.committed_chars = static_cast<int>(result.committed_text.size());
        result.latest_partial_chars = static_cast<int>(result.latest_partial_text.size());
        if (result.committed_text.empty() && !finalized_text_candidate_.empty()) {
            result.committed_text = finalized_text_candidate_;
            result.committed_chars = static_cast<int>(result.committed_text.size());
        }
    }

    if (!result.committed_text.empty()) {
        result.finalized = true;
        result.used_aggregate_text = true;
        result.final_text = result.committed_text;
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=ok source=aggregate chars=" + std::to_string(result.final_text.size()) +
                                             " waited_for_inflight_decode=" + std::string(result.waited_for_inflight_decode ? "true" : "false"));
        return result;
    }
    if (!result.latest_partial_text.empty()) {
        result.finalized = true;
        result.used_partial_fallback = true;
        result.final_text = result.latest_partial_text;
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=ok source=latest_partial chars=" + std::to_string(result.final_text.size()) +
                                             " waited_for_inflight_decode=" + std::string(result.waited_for_inflight_decode ? "true" : "false"));
        return result;
    }

    result.error = "No partial transcript available for streaming finalization.";
    result.finalized = false;
    pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=fail source=none waited_for_inflight_decode=" +
                                     std::string(result.waited_for_inflight_decode ? "true" : "false"), true);
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
