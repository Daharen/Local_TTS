#include "streaming_dictation.h"

#include "paths.h"
#include "pipeline_debug.h"
#include "whisper_runner.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
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
    std::size_t effective_overlap_tokens = 0;
    std::size_t edge_mismatches = 0;
    bool interior_mismatch = false;
    bool used_edge_softmatch = false;
    int shift_tokens = 0;
    bool anchor_used = false;
    std::size_t anchor_ngram_tokens = 0;
    std::size_t min_required_tokens = 0;
    std::size_t max_overlap_tokens_tried = 0;
    int first_interior_mismatch_index = -1;
    std::size_t reject_edge_mismatches = 0;
    std::string reject_prev_edge_token;
    std::string reject_curr_edge_token;
    std::string match_mode = "replace_window";
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
    return (std::max<std::size_t>)(4, pct_requirement);
}

bool is_edge_position(std::size_t index, std::size_t overlap_len) {
    return overlap_len > 0 && (index == 0 || index + 1 == overlap_len);
}

bool is_edge_soft_match_token(const std::string& left, const std::string& right) {
    if (left == right) {
        return true;
    }

    const std::size_t left_len = left.size();
    const std::size_t right_len = right.size();
    const std::size_t min_len = (std::min)(left_len, right_len);

    if (min_len >= 3) {
        if (left_len <= right_len && right.rfind(left, 0) == 0) {
            return true;
        }
        if (right_len <= left_len && left.rfind(right, 0) == 0) {
            return true;
        }
    }

    const std::size_t short_len_limit = 3;
    if ((left.empty() && right_len <= short_len_limit) || (right.empty() && left_len <= short_len_limit)) {
        return true;
    }

    return false;
}

std::size_t largest_anchor_ngram_len(const std::vector<TokenData>& prev_tokens,
                                     const std::vector<TokenData>& curr_tokens,
                                     std::size_t prev_start,
                                     std::size_t curr_start,
                                     std::size_t overlap_len) {
    if (overlap_len < 3) {
        return 0;
    }

    const std::size_t interior_start = (overlap_len > 2) ? 1 : 0;
    const std::size_t interior_end = (overlap_len > 2) ? overlap_len - 1 : overlap_len;
    if (interior_end <= interior_start) {
        return 0;
    }

    for (std::size_t n : {std::size_t(4), std::size_t(3)}) {
        if ((interior_end - interior_start) < n) {
            continue;
        }
        for (std::size_t i = interior_start; i + n <= interior_end; ++i) {
            bool exact = true;
            for (std::size_t j = 0; j < n; ++j) {
                if (prev_tokens[prev_start + i + j].normalized != curr_tokens[curr_start + i + j].normalized) {
                    exact = false;
                    break;
                }
            }
            if (exact) {
                return n;
            }
        }
    }
    return 0;
}

std::vector<int> overlap_shift_candidates() {
    return {0, -1, 1, -2, 2};
}

OverlapResult score_best_overlap(const std::vector<TokenData>& prev_tokens, const std::vector<TokenData>& curr_tokens) {
    OverlapResult best;
    const std::size_t max_overlap = (std::min)(prev_tokens.size(), curr_tokens.size());
    const std::size_t min_overlap = min_required_overlap_tokens(prev_tokens.size(), curr_tokens.size());
    best.min_required_tokens = min_overlap;
    best.max_overlap_tokens_tried = max_overlap;
    int first_interior_mismatch_index = std::numeric_limits<int>::max();
    std::size_t reject_edge_mismatches = 0;
    std::string reject_prev_edge_token;
    std::string reject_curr_edge_token;

    for (std::size_t k = max_overlap; k >= min_overlap && k > 0; --k) {
        const std::size_t prev_base = prev_tokens.size() - k;
        for (int shift : overlap_shift_candidates()) {
            const std::size_t shift_abs = static_cast<std::size_t>(std::abs(shift));
            if (shift_abs >= k) {
                continue;
            }

            const std::size_t effective_k = k - shift_abs;
            if (effective_k < min_overlap) {
                continue;
            }

            const std::size_t prev_shift = (shift > 0) ? static_cast<std::size_t>(shift) : 0;
            const std::size_t curr_shift = (shift < 0) ? static_cast<std::size_t>(-shift) : 0;
            const std::size_t prev_start = prev_base + prev_shift;
            const std::size_t curr_start = curr_shift;

            bool interior_mismatch = false;
            std::size_t edge_mismatches = 0;
            bool used_edge_softmatch = false;
            bool reject = false;
            bool all_exact = (shift == 0);

            for (std::size_t i = 0; i < effective_k; ++i) {
                const std::string& prev_norm = prev_tokens[prev_start + i].normalized;
                const std::string& curr_norm = curr_tokens[curr_start + i].normalized;
                if (prev_norm == curr_norm) {
                    continue;
                }

                all_exact = false;
                if (!is_edge_position(i, effective_k)) {
                    interior_mismatch = true;
                    reject = true;
                    if (static_cast<int>(i) < first_interior_mismatch_index) {
                        first_interior_mismatch_index = static_cast<int>(i);
                    }
                    break;
                }

                const bool soft_match = is_edge_soft_match_token(prev_norm, curr_norm);
                if (!soft_match) {
                    ++edge_mismatches;
                    reject_edge_mismatches = edge_mismatches;
                    reject_prev_edge_token = prev_tokens[prev_start + i].original;
                    reject_curr_edge_token = curr_tokens[curr_start + i].original;
                    if (edge_mismatches > 2) {
                        reject = true;
                        break;
                    }
                } else {
                    used_edge_softmatch = true;
                }
            }

            if (reject || interior_mismatch) {
                continue;
            }

            const std::size_t anchor_len = largest_anchor_ngram_len(prev_tokens, curr_tokens, prev_start, curr_start, effective_k);
            const bool anchor_used = anchor_len >= 3;
            const bool exact_mode = all_exact && edge_mismatches == 0 && !used_edge_softmatch;
            if (!exact_mode && !anchor_used) {
                continue;
            }

            best.match_ok = true;
            best.overlap_tokens = k;
            best.effective_overlap_tokens = effective_k;
            best.promoted_prefix_tokens = prev_start;
            best.edge_mismatches = edge_mismatches;
            best.interior_mismatch = false;
            best.used_edge_softmatch = used_edge_softmatch;
            best.shift_tokens = shift;
            best.anchor_used = anchor_used;
            best.anchor_ngram_tokens = anchor_len;
            best.first_interior_mismatch_index = -1;
            best.reject_edge_mismatches = 0;
            best.reject_prev_edge_token.clear();
            best.reject_curr_edge_token.clear();
            best.match_mode = exact_mode ? "exact" : "edge_tolerant";
            return best;
        }
    }

    best.match_ok = false;
    best.overlap_tokens = 0;
    best.promoted_prefix_tokens = 0;
    best.effective_overlap_tokens = 0;
    best.edge_mismatches = 0;
    best.interior_mismatch = (first_interior_mismatch_index != std::numeric_limits<int>::max());
    best.used_edge_softmatch = false;
    best.shift_tokens = 0;
    best.anchor_used = false;
    best.anchor_ngram_tokens = 0;
    best.first_interior_mismatch_index =
        (first_interior_mismatch_index == std::numeric_limits<int>::max()) ? -1 : first_interior_mismatch_index;
    best.reject_edge_mismatches = reject_edge_mismatches;
    best.reject_prev_edge_token = reject_prev_edge_token;
    best.reject_curr_edge_token = reject_curr_edge_token;
    best.match_mode = "replace_window";
    return best;
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

    result = score_best_overlap(prev_tokens, curr_tokens);
    if (!result.match_ok) {
        return result;
    }

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
            std::size_t overlap_effective_tokens = 0;
            std::size_t promoted_prefix_tokens = 0;
            std::size_t overlap_edge_mismatches = 0;
            bool overlap_interior_mismatch = false;
            bool overlap_edge_softmatch = false;
            int overlap_shift_tokens = 0;
            bool overlap_anchor_used = false;
            std::size_t overlap_anchor_tokens = 0;
            std::size_t overlap_min_required_tokens = 0;
            std::size_t overlap_max_tried_tokens = 0;
            int overlap_first_interior_mismatch_index = -1;
            std::size_t overlap_reject_edge_mismatches = 0;
            std::string overlap_reject_prev_edge_token;
            std::string overlap_reject_curr_edge_token;
            std::string overlap_match_mode = "replace_window";
            if (!previous_window_text_.empty()) {
                OverlapResult overlap_result =
                    promote_window_prefix(committed_prefix_text_, previous_window_text_, current_window_text);
                overlap_match_ok = overlap_result.match_ok;
                overlap_tokens = overlap_result.overlap_tokens;
                overlap_effective_tokens = overlap_result.effective_overlap_tokens;
                promoted_prefix_tokens = overlap_result.promoted_prefix_tokens;
                overlap_edge_mismatches = overlap_result.edge_mismatches;
                overlap_interior_mismatch = overlap_result.interior_mismatch;
                overlap_edge_softmatch = overlap_result.used_edge_softmatch;
                overlap_shift_tokens = overlap_result.shift_tokens;
                overlap_anchor_used = overlap_result.anchor_used;
                overlap_anchor_tokens = overlap_result.anchor_ngram_tokens;
                overlap_min_required_tokens = overlap_result.min_required_tokens;
                overlap_max_tried_tokens = overlap_result.max_overlap_tokens_tried;
                overlap_first_interior_mismatch_index = overlap_result.first_interior_mismatch_index;
                overlap_reject_edge_mismatches = overlap_result.reject_edge_mismatches;
                overlap_reject_prev_edge_token = overlap_result.reject_prev_edge_token;
                overlap_reject_curr_edge_token = overlap_result.reject_curr_edge_token;
                overlap_match_mode = overlap_result.match_mode;
                if (!overlap_match_ok) {
                    ++overlap_failure_count_;
                    const std::string prev_clean = collapse_whitespace(previous_window_text_);
                    const auto prev_tokens = tokenize_for_merge(prev_clean);
                    const auto curr_tokens = tokenize_for_merge(current_window_text);
                    latest_window_text_ = (curr_tokens.size() >= prev_tokens.size()) ? current_window_text : prev_clean;
                    pipeline_debug::log("streaming",
                                        "[STREAM_OVERLAP_WARNING] match=false action=replace_window_without_append",
                                        true);
                    pipeline_debug::log("streaming",
                                        "[STREAM_OVERLAP_REJECT_DETAIL] min_required=" +
                                            std::to_string(overlap_min_required_tokens) +
                                            " max_tried=" + std::to_string(overlap_max_tried_tokens) +
                                            " first_interior_idx=" + std::to_string(overlap_first_interior_mismatch_index) +
                                            " edge_mismatches=" + std::to_string(overlap_reject_edge_mismatches) +
                                            " prev_edge_token=\"" + overlap_reject_prev_edge_token +
                                            "\" curr_edge_token=\"" + overlap_reject_curr_edge_token + "\"",
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
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_EFFECTIVE_TOKENS] n=" + std::to_string(overlap_effective_tokens));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_EDGE_MISMATCHES] n=" + std::to_string(overlap_edge_mismatches));
            pipeline_debug::log("streaming",
                                "[STREAM_OVERLAP_INTERIOR_MISMATCH] " +
                                    std::string(overlap_interior_mismatch ? "true" : "false"));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_EDGE_SOFTMATCH] " + std::string(overlap_edge_softmatch ? "true" : "false"));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_SHIFT] n=" + std::to_string(overlap_shift_tokens));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_ANCHOR_USED] " + std::string(overlap_anchor_used ? "true" : "false"));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_ANCHOR_N] n=" + std::to_string(overlap_anchor_tokens));
            pipeline_debug::log("streaming", "[STREAM_OVERLAP_MATCH_MODE] " + overlap_match_mode);
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
