#pragma once

#include <filesystem>
#include <string>

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
};

int run_whisper_file_transcription(const std::filesystem::path& audio_path);
bool transcribe_file_to_string(const std::filesystem::path& audio_path, std::string& text_out, std::string& error_out);
bool transcribe_file_to_string_with_info(
    const std::filesystem::path& audio_path,
    std::string& text_out,
    std::string& error_out,
    WhisperRunInfo* info_out);
