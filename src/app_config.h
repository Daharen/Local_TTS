#pragma once

#include <filesystem>
#include <string>

struct AppConfig {
    std::filesystem::path large_data_root;
    std::filesystem::path whisper_cpp_root;
    std::filesystem::path whisper_model_path;
    std::filesystem::path llama_cpp_root;
    std::filesystem::path llama_model_path;

    bool correction_enabled;
    double correction_temperature;
    int correction_top_k;
    double correction_top_p;
    double correction_min_p;
    std::string correction_mode;
    int correction_max_output_tokens;
    int correction_segment_max_chars;
    int correction_segment_overlap_chars;
    int correction_force_segmentation_threshold_chars;
};

const AppConfig& get_app_config();

