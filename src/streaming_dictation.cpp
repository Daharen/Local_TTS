#include "streaming_dictation.h"

#include "paths.h"
#include "pipeline_debug.h"
#include "whisper_runner.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>

namespace {

using Clock = std::chrono::steady_clock;

struct ConfirmedPrefixResult {
    std::size_t match_tokens = 0;
    std::size_t newly_confirmed_tokens = 0;
};

std::string collapse_whitespace(const std::string& text) {
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

void tokenize_text(const std::string& text, std::vector<std::string>& original, std::vector<std::string>& normalized) {
    original.clear();
    normalized.clear();
    std::istringstream in(text);
    std::string token;
    while (in >> token) {
        const std::string normalized_token = normalize_token_for_match(token);
        if (normalized_token.empty()) {
            continue;
        }
        original.push_back(token);
        normalized.push_back(normalized_token);
    }
}

std::string join_original_tokens(const std::vector<std::string>& tokens, std::size_t start, std::size_t end) {
    if (start >= end || start >= tokens.size()) {
        return {};
    }
    std::ostringstream out;
    const std::size_t bounded_end = (std::min)(end, tokens.size());
    for (std::size_t i = start; i < bounded_end; ++i) {
        if (i > start) {
            out << ' ';
        }
        out << tokens[i];
    }
    return out.str();
}

std::string rebuild_from_token_parts(const std::string& committed, const std::string& unconfirmed) {
    if (committed.empty()) {
        return unconfirmed;
    }
    if (unconfirmed.empty()) {
        return committed;
    }
    return committed + " " + unconfirmed;
}

ConfirmedPrefixResult compute_confirmed_prefix(
    const std::deque<StreamingHypothesis>& history,
    const std::vector<std::string>& already_committed_normalized_tokens) {
    ConfirmedPrefixResult result;
    if (history.empty()) {
        return result;
    }
    const std::size_t skip = already_committed_normalized_tokens.size();
    std::size_t common = static_cast<std::size_t>(-1);
    for (const auto& hyp : history) {
        if (hyp.tokens_normalized.size() <= skip) {
            common = 0;
            break;
        }
        const std::size_t available = hyp.tokens_normalized.size() - skip;
        common = (common == static_cast<std::size_t>(-1)) ? available : (std::min)(common, available);
    }
    if (common == static_cast<std::size_t>(-1)) {
        common = 0;
    }
    for (std::size_t i = 0; i < common; ++i) {
        const std::string& anchor = history.front().tokens_normalized[skip + i];
        for (std::size_t h = 1; h < history.size(); ++h) {
            if (history[h].tokens_normalized[skip + i] != anchor) {
                common = i;
                result.match_tokens = skip + i;
                result.newly_confirmed_tokens = i;
                return result;
            }
        }
    }
    result.match_tokens = skip + common;
    result.newly_confirmed_tokens = common;
    return result;
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
    local_agreement_n_ = (std::max)(2, get_stream_local_agreement_n());
    prompt_max_tokens_ = (std::max)(1, get_stream_prompt_max_tokens());
    trim_guard_ms_ = (std::max)(0, get_stream_trim_guard_ms());
    trim_on_segment_boundary_ = is_stream_trim_on_segment_boundary_enabled();

    session_id_ = session_id;
    stop_requested_.store(false);
    capture_started_at_ = Clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        hypothesis_history_.clear();
        committed_prefix_text_.clear();
        committed_prefix_tokens_original_.clear();
        committed_prefix_tokens_normalized_.clear();
        committed_prompt_tokens_.clear();
        latest_unconfirmed_text_.clear();
        final_candidate_text_.clear();
        committed_audio_ms_ = 0;
        audio_buffer_base_ms_ = 0;
        agreement_failure_count_ = 0;
        local_agreement_match_tokens_ = 0;
        commit_advance_tokens_ = 0;
        latest_audio_trim_ms_ = 0;
        latest_segment_trim_used_ = false;
        decode_in_progress_ = false;
        decode_iteration_count_ = 0;
        stream_first_partial_ms_ = -1;
    }

    pipeline_debug::log("streaming", "[STREAM_BEGIN] session_id=" + std::to_string(session_id_) +
                                         " stream_step_ms=" + std::to_string(step_ms_) +
                                         " stream_length_ms=" + std::to_string(length_ms_) +
                                         " stream_keep_ms=" + std::to_string(keep_ms_) +
                                         " stream_local_agreement_n=" + std::to_string(local_agreement_n_));
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

        int iteration = 0;
        int buffer_base_ms_snapshot = 0;
        std::vector<int> prompt_tokens_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++decode_iteration_count_;
            iteration = decode_iteration_count_;
            buffer_base_ms_snapshot = audio_buffer_base_ms_;
            prompt_tokens_snapshot = committed_prompt_tokens_;
            committed_prompt_tokens_.clear();
            decode_in_progress_ = true;
        }

        WhisperDecodeOptions options;
        options.enable_timestamps = true;
        options.single_segment = false;
        options.no_context = false;
        options.prompt_tokens = prompt_tokens_snapshot.empty() ? nullptr : prompt_tokens_snapshot.data();
        options.prompt_token_count = static_cast<int>(prompt_tokens_snapshot.size());

        WhisperDecodeResult decode_result;
        WhisperRunInfo info;
        std::string error;
        const bool ok = transcribe_pcm_to_result_with_info(
            window_pcm.data(), static_cast<int>(window_pcm.size()), options, decode_result, error, &info);
        if (!ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            decode_in_progress_ = false;
            pipeline_debug::log("streaming", "[STREAM_DECODE_END] iter=" + std::to_string(iteration) + " status=fail error=" + error, true);
            continue;
        }

        StreamingHypothesis hypothesis;
        hypothesis.decode_index = static_cast<uint64_t>(iteration);
        hypothesis.full_text = collapse_whitespace(decode_result.text);
        tokenize_text(hypothesis.full_text, hypothesis.tokens_original, hypothesis.tokens_normalized);
        hypothesis.whisper_prompt_tokens = decode_result.prompt_tokens_from_output;
        for (const auto& segment : decode_result.segments) {
            StreamingSegment local_segment;
            local_segment.text = collapse_whitespace(segment.text);
            local_segment.t0_ms = segment.t0_ms;
            local_segment.t1_ms = segment.t1_ms;
            local_segment.token_ids = segment.token_ids;
            hypothesis.segments.push_back(std::move(local_segment));
        }
        const int window_ms = static_cast<int>((window_pcm.size() * 1000) / AudioCapture::kSampleRate);
        hypothesis.audio_buffer_start_ms = buffer_base_ms_snapshot;
        hypothesis.audio_buffer_end_ms = buffer_base_ms_snapshot + window_ms;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            hypothesis_history_.push_back(std::move(hypothesis));
            while (static_cast<int>(hypothesis_history_.size()) > local_agreement_n_) {
                hypothesis_history_.pop_front();
            }

            const bool have_full_agreement_history = static_cast<int>(hypothesis_history_.size()) >= local_agreement_n_;
            ConfirmedPrefixResult agreement;
            if (have_full_agreement_history) {
                agreement = compute_confirmed_prefix(hypothesis_history_, committed_prefix_tokens_normalized_);
                local_agreement_match_tokens_ = static_cast<int>(agreement.match_tokens);
                commit_advance_tokens_ = static_cast<int>(agreement.newly_confirmed_tokens);
            } else {
                agreement.match_tokens = committed_prefix_tokens_normalized_.size();
                agreement.newly_confirmed_tokens = 0;
                local_agreement_match_tokens_ = static_cast<int>(agreement.match_tokens);
                commit_advance_tokens_ = 0;
            }

            if (agreement.newly_confirmed_tokens == 0 && hypothesis_history_.size() >= 2) {
                ++agreement_failure_count_;
            }

            if (agreement.newly_confirmed_tokens > 0 && !hypothesis_history_.empty()) {
                const auto& latest = hypothesis_history_.back();
                const std::size_t start = committed_prefix_tokens_original_.size();
                const std::size_t requested_end = start + agreement.newly_confirmed_tokens;
                const std::size_t end = (std::min)(requested_end, latest.tokens_original.size());
                const std::size_t normalized_end = (std::min)(requested_end, latest.tokens_normalized.size());
                const std::size_t actual_added = (end > start && normalized_end > start) ? (std::min)(end - start, normalized_end - start) : 0;
                commit_advance_tokens_ = static_cast<int>(actual_added);
                if (end <= start || normalized_end <= start) {
                    commit_advance_tokens_ = 0;
                }
                const std::string newly_confirmed = join_original_tokens(latest.tokens_original, start, end);
                if (!newly_confirmed.empty()) {
                    committed_prefix_text_ = collapse_whitespace(rebuild_from_token_parts(committed_prefix_text_, newly_confirmed));
                }
                if (end > start && normalized_end > start) {
                    committed_prefix_tokens_original_.insert(committed_prefix_tokens_original_.end(),
                                                             latest.tokens_original.begin() + static_cast<std::ptrdiff_t>(start),
                                                             latest.tokens_original.begin() + static_cast<std::ptrdiff_t>(end));
                    committed_prefix_tokens_normalized_.insert(committed_prefix_tokens_normalized_.end(),
                                                               latest.tokens_normalized.begin() + static_cast<std::ptrdiff_t>(start),
                                                               latest.tokens_normalized.begin() + static_cast<std::ptrdiff_t>(normalized_end));
                }

                latest_segment_trim_used_ = false;
                latest_audio_trim_ms_ = 0;
                if (trim_on_segment_boundary_) {
                    int candidate_trim_ms = committed_audio_ms_;
                    std::size_t committed_boundary_tokens = 0;
                    std::vector<int> prompt_candidate_tokens;
                    for (const auto& segment : latest.segments) {
                        if (segment.t1_ms <= 0) {
                            continue;
                        }
                        std::vector<std::string> segment_normalized;
                        std::vector<std::string> segment_original_unused;
                        tokenize_text(segment.text, segment_original_unused, segment_normalized);
                        if (segment_normalized.empty()) {
                            continue;
                        }
                        committed_boundary_tokens += segment_normalized.size();
                        if (committed_prefix_tokens_normalized_.size() >= committed_boundary_tokens) {
                            candidate_trim_ms = (std::max)(candidate_trim_ms, segment.t1_ms);
                            latest_segment_trim_used_ = true;
                            prompt_candidate_tokens.insert(prompt_candidate_tokens.end(), segment.token_ids.begin(), segment.token_ids.end());
                        } else {
                            break;
                        }
                    }
                    if (!prompt_candidate_tokens.empty()) {
                        committed_prompt_tokens_ = std::move(prompt_candidate_tokens);
                        if (static_cast<int>(committed_prompt_tokens_.size()) > prompt_max_tokens_) {
                            committed_prompt_tokens_.erase(
                                committed_prompt_tokens_.begin(),
                                committed_prompt_tokens_.begin() +
                                    (static_cast<std::ptrdiff_t>(committed_prompt_tokens_.size()) - prompt_max_tokens_));
                        }
                    }
                    if (candidate_trim_ms > committed_audio_ms_) {
                        committed_audio_ms_ = candidate_trim_ms;
                    }
                    const int guarded = (std::max)(0, committed_audio_ms_ - trim_guard_ms_);
                    if (guarded > audio_buffer_base_ms_) {
                        latest_audio_trim_ms_ = guarded - audio_buffer_base_ms_;
                        audio_buffer_base_ms_ = guarded;
                    }
                }
                if (committed_prompt_tokens_.empty() && !latest.whisper_prompt_tokens.empty()) {
                    committed_prompt_tokens_ = latest.whisper_prompt_tokens;
                    if (static_cast<int>(committed_prompt_tokens_.size()) > prompt_max_tokens_) {
                        committed_prompt_tokens_.erase(
                            committed_prompt_tokens_.begin(),
                            committed_prompt_tokens_.begin() +
                                (static_cast<std::ptrdiff_t>(committed_prompt_tokens_.size()) - prompt_max_tokens_));
                    }
                }
            }

            if (!hypothesis_history_.empty()) {
                const auto& latest = hypothesis_history_.back();
                const std::size_t committed_tokens = committed_prefix_tokens_original_.size();
                latest_unconfirmed_text_ = join_original_tokens(
                    latest.tokens_original, committed_tokens, latest.tokens_original.size());
                latest_unconfirmed_text_ = collapse_whitespace(latest_unconfirmed_text_);
            } else {
                latest_unconfirmed_text_.clear();
            }
            final_candidate_text_ = collapse_whitespace(rebuild_from_token_parts(committed_prefix_text_, latest_unconfirmed_text_));

            decode_in_progress_ = false;
            if (stream_first_partial_ms_ < 0) {
                stream_first_partial_ms_ =
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - capture_started_at_).count();
            }

            pipeline_debug::log("streaming", "[STREAM_AGREEMENT_N] " + std::to_string(local_agreement_n_));
            pipeline_debug::log("streaming", "[STREAM_HISTORY_DEPTH] " + std::to_string(hypothesis_history_.size()));
            pipeline_debug::log("streaming",
                                "[STREAM_COMMIT_DEFERRED_HISTORY] " + std::string(have_full_agreement_history ? "false" : "true"));
            pipeline_debug::log("streaming", "[STREAM_HYPOTHESIS_CHARS] " +
                                                 std::to_string(hypothesis_history_.empty() ? 0 : hypothesis_history_.back().full_text.size()));
            pipeline_debug::log("streaming", "[STREAM_CONFIRMED_PREFIX_TOKENS] " +
                                                 std::to_string(committed_prefix_tokens_normalized_.size()));
            pipeline_debug::log("streaming", "[STREAM_COMMIT_ADVANCE_TOKENS] " + std::to_string(commit_advance_tokens_));
            pipeline_debug::log("streaming", "[STREAM_COMMITTED_PREFIX_CHARS] " + std::to_string(committed_prefix_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_UNCONFIRMED_SUFFIX_CHARS] " + std::to_string(latest_unconfirmed_text_.size()));
            pipeline_debug::log("streaming", "[STREAM_PROMPT_TOKENS] " + std::to_string(committed_prompt_tokens_.size()));
            pipeline_debug::log("streaming", "[STREAM_AUDIO_TRIM_MS] " + std::to_string(latest_audio_trim_ms_));
            pipeline_debug::log("streaming", "[STREAM_BUFFER_BASE_MS] " + std::to_string(audio_buffer_base_ms_));
            pipeline_debug::log("streaming",
                                "[STREAM_SEGMENT_TRIM_USED] " + std::string(latest_segment_trim_used_ ? "true" : "false"));
            pipeline_debug::log("streaming",
                                "[STREAM_LOCAL_AGREEMENT_MATCH_TOKENS] " + std::to_string(local_agreement_match_tokens_));
            pipeline_debug::log("streaming",
                                "[STREAM_DECODE_END] iter=" + std::to_string(iteration) + " status=ok infer_ms=" +
                                    std::to_string(info.infer_ms));
        }
    }
}

StreamingDictationResult StreamingDictationSession::finalize() {
    StreamingDictationResult result;
    if (!is_active_.exchange(false)) {
        result.error = "streaming session was not active";
        return result;
    }

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
        result.decode_iteration_count = decode_iteration_count_;
        result.committed_prefix_text = committed_prefix_text_;
        result.committed_text = committed_prefix_text_;
        result.latest_partial_text = latest_unconfirmed_text_;
        result.final_candidate_text = final_candidate_text_;
        result.stream_first_partial_ms = stream_first_partial_ms_;
        result.stream_agreement_n = local_agreement_n_;
        result.stream_history_depth = static_cast<int>(hypothesis_history_.size());
        result.stream_hypothesis_chars = hypothesis_history_.empty() ? 0 : static_cast<int>(hypothesis_history_.back().full_text.size());
        result.stream_confirmed_prefix_tokens = static_cast<int>(committed_prefix_tokens_normalized_.size());
        result.stream_commit_advance_tokens = commit_advance_tokens_;
        result.stream_unconfirmed_suffix_chars = static_cast<int>(latest_unconfirmed_text_.size());
        result.stream_prompt_tokens = static_cast<int>(committed_prompt_tokens_.size());
        result.stream_audio_trim_ms = latest_audio_trim_ms_;
        result.stream_buffer_base_ms = audio_buffer_base_ms_;
        result.stream_segment_trim_used = latest_segment_trim_used_;
        result.stream_local_agreement_match_tokens = local_agreement_match_tokens_;
        result.committed_prefix_chars = static_cast<int>(result.committed_prefix_text.size());
        result.latest_partial_chars = static_cast<int>(result.latest_partial_text.size());
        result.final_candidate_chars = static_cast<int>(result.final_candidate_text.size());
        result.committed_chars = result.committed_prefix_chars;
    }

    result.stream_final_infer_ms = 0.0;
    if (!result.final_candidate_text.empty()) {
        result.finalized = true;
        result.final_text = result.final_candidate_text;
    } else if (!result.committed_prefix_text.empty()) {
        result.finalized = true;
        result.final_text = result.committed_prefix_text;
    } else {
        result.error = "No partial transcript available for streaming finalization.";
        result.finalized = false;
    }
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
