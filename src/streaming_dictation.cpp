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

struct OverlapResult {
    bool match_ok = false;
    std::size_t overlap_tokens = 0;
    std::size_t promoted_prefix_tokens = 0;
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

std::string join_token_range(const std::vector<TokenData>& tokens, std::size_t start, std::size_t end = std::string::npos) {
    const std::size_t bounded_end = (end == std::string::npos) ? tokens.size() : (std::min)(end, tokens.size());
    if (start >= bounded_end) {
        return {};
    }
    std::ostringstream out;
    for (std::size_t i = start; i < bounded_end; ++i) {
        if (i > start) {
            out << ' ';
        }
        out << tokens[i].original;
    }
    return out.str();
}

std::size_t best_suffix_prefix_overlap_tokens(const std::vector<TokenData>& prev, const std::vector<TokenData>& curr) {
    const std::size_t max_overlap = (std::min)(prev.size(), curr.size());
    for (std::size_t k = max_overlap; k > 0; --k) {
        bool overlap = true;
        for (std::size_t i = 0; i < k; ++i) {
            if (prev[prev.size() - k + i].normalized != curr[i].normalized) {
                overlap = false;
                break;
            }
        }
        if (overlap) {
            return k;
        }
    }
    return 0;
}

std::size_t min_required_overlap_tokens(std::size_t prev_count, std::size_t curr_count) {
    const std::size_t shorter = (std::min)(prev_count, curr_count);
    const std::size_t pct_requirement = (shorter + 4) / 5;  // ceil(20%)
    return (std::max<std::size_t>)(3, pct_requirement);
}

OverlapResult promote_window_prefix(std::string& committed_prefix_text,
                                    const std::string& previous_window_text,
                                    const std::string& current_window_text) {
    OverlapResult result;
    const std::string prev_clean = collapse_whitespace(previous_window_text);
    const std::string curr_clean = collapse_whitespace(current_window_text);
    if (prev_clean.empty() || curr_clean.empty()) {
        return result;
    }

    const auto prev_tokens = tokenize_for_merge(prev_clean);
    const auto curr_tokens = tokenize_for_merge(curr_clean);
    if (prev_tokens.empty() || curr_tokens.empty()) {
        return result;
    }

    const std::size_t best_overlap = best_suffix_prefix_overlap_tokens(prev_tokens, curr_tokens);
    const std::size_t min_overlap = min_required_overlap_tokens(prev_tokens.size(), curr_tokens.size());
    if (best_overlap < min_overlap) {
        return result;
    }

    result.match_ok = true;
    result.overlap_tokens = best_overlap;
    result.promoted_prefix_tokens = prev_tokens.size() - best_overlap;
    const std::string promoted_prefix = join_token_range(prev_tokens, 0, result.promoted_prefix_tokens);
    if (!promoted_prefix.empty()) {
        if (!committed_prefix_text.empty()) {
            committed_prefix_text.push_back(' ');
        }
        committed_prefix_text += promoted_prefix;
        committed_prefix_text = collapse_whitespace(committed_prefix_text);
    }
    return result;
}

std::string merge_committed_prefix_with_window(const std::string& committed_prefix, const std::string& latest_window) {
    const std::string committed_clean = collapse_whitespace(committed_prefix);
    const std::string window_clean = collapse_whitespace(latest_window);
    if (committed_clean.empty()) {
        return window_clean;
    }
    if (window_clean.empty()) {
        return committed_clean;
    }

    const auto committed_tokens = tokenize_for_merge(committed_clean);
    const auto window_tokens = tokenize_for_merge(window_clean);
    if (committed_tokens.empty()) {
        return window_clean;
    }
    if (window_tokens.empty()) {
        return committed_clean;
    }

    const std::size_t overlap = best_suffix_prefix_overlap_tokens(committed_tokens, window_tokens);
    std::string merged = committed_clean;
    const std::string window_tail = join_token_range(window_tokens, overlap);
    if (!window_tail.empty()) {
        merged.push_back(' ');
        merged += window_tail;
    }
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
        committed_prefix_text_.clear();
        previous_window_text_.clear();
        latest_window_text_.clear();
        latest_partial_text_.clear();
        finalized_text_candidate_.clear();
        overlap_failure_count_ = 0;
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
            const std::string current_window_text = collapse_whitespace(partial);
            if (!latest_window_text_.empty()) {
                previous_window_text_ = latest_window_text_;
            }
            bool overlap_match_ok = false;
            std::size_t overlap_tokens = 0;
            std::size_t promoted_prefix_tokens = 0;
            if (!previous_window_text_.empty()) {
                OverlapResult overlap_result =
                    promote_window_prefix(committed_prefix_text_, previous_window_text_, current_window_text);
                overlap_match_ok = overlap_result.match_ok;
                overlap_tokens = overlap_result.overlap_tokens;
                promoted_prefix_tokens = overlap_result.promoted_prefix_tokens;
                if (!overlap_match_ok) {
                    ++overlap_failure_count_;
                    const std::string prev_clean = collapse_whitespace(previous_window_text_);
                    const auto prev_tokens = tokenize_for_merge(prev_clean);
                    const auto curr_tokens = tokenize_for_merge(current_window_text);
                    latest_window_text_ = (curr_tokens.size() >= prev_tokens.size()) ? current_window_text : prev_clean;
                    pipeline_debug::log("streaming",
                                        "[STREAM_OVERLAP_WARNING] match=false action=replace_window_without_append",
                                        true);
                } else {
                    latest_window_text_ = current_window_text;
                }
            } else {
                latest_window_text_ = current_window_text;
            }
            latest_partial_text_ = latest_window_text_;
            committed_text_ = committed_prefix_text_;
            finalized_text_candidate_ = merge_committed_prefix_with_window(committed_prefix_text_, latest_window_text_);
            decode_in_progress_ = false;
            if (stream_first_partial_ms_ < 0) {
                stream_first_partial_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(last_decode_time_ - capture_started_at_).count();
            }
            pipeline_debug::log("streaming", "[STREAM_PARTIAL_TEXT] chars=" + std::to_string(latest_partial_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_MATCH_OK] " + std::string(overlap_match_ok ? "true" : "false"));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_TOKENS] n=" + std::to_string(overlap_tokens));
            pipeline_debug::log("streaming", "[STREAM_PROMOTED_PREFIX_TOKENS] n=" + std::to_string(promoted_prefix_tokens));
            pipeline_debug::log("streaming", "[STREAM_COMMITTED_PREFIX_CHARS] " + std::to_string(committed_prefix_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_WINDOW_CHARS] " + std::to_string(latest_window_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_FINAL_CANDIDATE_CHARS] " + std::to_string(finalized_text_candidate_.size()));
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
        result.committed_prefix_text = committed_prefix_text_;
        result.latest_window_text = latest_window_text_;
        result.final_candidate_text = finalized_text_candidate_;
        result.committed_text = committed_prefix_text_;
        result.latest_partial_text = latest_window_text_;
        result.decode_iteration_count = decode_iteration_count_;
        result.stream_first_partial_ms = stream_first_partial_ms_;
        result.overlap_failure_count = overlap_failure_count_;
        result.committed_prefix_chars = static_cast<int>(result.committed_prefix_text.size());
        result.latest_window_chars = static_cast<int>(result.latest_window_text.size());
        result.final_candidate_chars = static_cast<int>(result.final_candidate_text.size());
        result.committed_chars = result.committed_prefix_chars;
        result.latest_partial_chars = result.latest_window_chars;
    }

    if (!result.final_candidate_text.empty()) {
        result.finalized = true;
        result.used_aggregate_text = true;
        result.final_text = result.final_candidate_text;
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=ok source=aggregate chars=" + std::to_string(result.final_text.size()) +
                                             " waited_for_inflight_decode=" + std::string(result.waited_for_inflight_decode ? "true" : "false"));
        return result;
    }
    const std::string combined_fallback =
        merge_committed_prefix_with_window(result.committed_prefix_text, result.latest_window_text);
    if (!combined_fallback.empty()) {
        result.finalized = true;
        result.used_aggregate_text = true;
        result.final_text = combined_fallback;
        pipeline_debug::log("streaming", "[STREAM_FINALIZE_END] status=ok source=aggregate chars=" + std::to_string(result.final_text.size()) +
                                             " waited_for_inflight_decode=" + std::string(result.waited_for_inflight_decode ? "true" : "false"));
        return result;
    }
    if (!result.latest_window_text.empty()) {
        result.finalized = true;
        result.used_partial_fallback = true;
        result.final_text = result.latest_window_text;
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
