#pragma once

#include <filesystem>
#include <string>

struct AppConfig {
    std::filesystem::path large_data_root;
    std::filesystem::path whisper_cpp_root;
    std::filesystem::path whisper_model_path;
    std::filesystem::path whisper_cli_path;
    bool whisper_use_gpu;
    int whisper_gpu_device;
    bool whisper_flash_attn;
    int whisper_threads;
    std::filesystem::path llama_cpp_root;
    std::filesystem::path llama_model_path;
    bool pipeline_debug_enabled;

    bool correction_enabled;
    double correction_temperature;
    int correction_top_k;
    double correction_top_p;
    double correction_min_p;
    std::string correction_mode;
    std::string correction_backend_mode;
    bool correction_resident_enabled;
    std::string correction_resident_host;
    int correction_resident_port;
    int correction_resident_ctx_size;
    int correction_resident_gpu_layers;
    int correction_resident_threads;
    int correction_resident_startup_timeout_ms;
    int correction_resident_request_timeout_ms;
    int correction_resident_total_budget_ms;
    int correction_resident_per_attempt_timeout_ms;
    int correction_max_output_tokens;
    int correction_segment_max_chars;
    int correction_segment_overlap_chars;
    int correction_force_segmentation_threshold_chars;
    bool streaming_enabled;
    int stream_step_ms;
    int stream_length_ms;
    int stream_keep_ms;
    bool stream_finalize_on_release;
};

const AppConfig& get_app_config();
