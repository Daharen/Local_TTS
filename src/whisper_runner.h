#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct WhisperRunInfo {
    std::string resolved_whisper_executable;
    std::string resolved_model_path;
    std::string argument_excerpt;
    std::string stdout_excerpt;
    std::string stderr_excerpt;
    int exit_code = -1;
    bool gpu_requested = false;
    bool gpu_active = false;
    bool cpu_fallback_reported = false;
    std::string backend_summary;
    std::string timing_excerpt;
    double init_ms = 0.0;
    double wav_ms = 0.0;
    double infer_ms = 0.0;
    double extract_ms = 0.0;
    double total_ms = 0.0;
};

struct WhisperSegmentResult {
    std::string text;
    int t0_ms = 0;
    int t1_ms = 0;
    std::vector<int> token_ids;
};

struct WhisperDecodeResult {
    std::string text;
    std::vector<WhisperSegmentResult> segments;
    std::vector<int> prompt_tokens_from_output;
};

struct WhisperDecodeOptions {
    bool enable_timestamps = true;
    bool single_segment = false;
    bool no_context = false;
    const int* prompt_tokens = nullptr;
    int prompt_token_count = 0;
    int max_tokens = 0;
};

int run_whisper_file_transcription(const std::filesystem::path& audio_path);
bool transcribe_file_to_string(const std::filesystem::path& audio_path, std::string& text_out, std::string& error_out);
bool transcribe_file_to_string_with_info(
    const std::filesystem::path& audio_path,
    std::string& text_out,
    std::string& error_out,
    WhisperRunInfo* info_out);
bool transcribe_pcm_to_string_with_info(
    const float* pcm,
    int pcm_sample_count,
    std::string& text_out,
    std::string& error_out,
    WhisperRunInfo* info_out);
bool transcribe_pcm_to_result_with_info(
    const float* pcm,
    int pcm_sample_count,
    const WhisperDecodeOptions& options,
    WhisperDecodeResult& result_out,
    std::string& error_out,
    WhisperRunInfo* info_out);
