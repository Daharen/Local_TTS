#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct CorrectionRunInfo {
    std::filesystem::path llama_exe;
    std::filesystem::path llama_model;
    std::string correction_mode;
    std::string backend_used;
    std::string raw_stdout_excerpt;
    std::string raw_stderr_excerpt;
    std::string raw_error_text;
    std::string sanitized_output;
    std::string sanitizer_reason;
    bool resident_attempted = false;
    bool resident_started = false;
    bool resident_health_check_ok = false;
    bool resident_request_sent = false;
    int resident_http_status = 0;
    bool fallback_used = false;
    std::string resident_phase;
    int resident_total_budget_ms = 0;
    int resident_remaining_budget_ms = 0;
    int resident_attempt_timeout_ms = 0;
    int resident_request_count = 0;
    int resident_last_status = 0;
    std::string resident_error;
    std::string resident_last_error;
    std::string resident_reset_reason;
    std::string resident_startup_error;
    std::string resident_server_exe;
    std::string resident_last_endpoint;
    std::string resident_endpoint_used;
    std::string resident_probe_used;
    std::string resident_args_excerpt;
    std::string resident_probe_response_excerpt;
    int resident_gpu_layers = 0;
    int resident_ctx_size = 0;
    int resident_threads = 0;
    bool oneshot_stderr_cuda_hint = false;
    std::string raw_stdout;
    std::string clean_output;
    bool segmented = false;
    int segment_count = 0;
    int max_output_tokens = 0;
    std::vector<int> failed_segment_indices;
};

bool correct_transcript_text(const std::string& raw_text, std::string& corrected_text, std::string& error_out);
bool correct_transcript_text_with_info(const std::string& raw_text,
                                       std::string& corrected_text,
                                       std::string& error_out,
                                       CorrectionRunInfo* info_out);
bool ensure_llm_correction_backend_ready(CorrectionRunInfo* info_out, std::string& error_out);
int run_llm_test_command(const std::string& input_text);
void shutdown_llm_correction_backend();
