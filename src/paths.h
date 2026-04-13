#pragma once

#include <filesystem>
#include <string>

std::filesystem::path get_repo_root();
std::filesystem::path get_large_data_root();
std::filesystem::path get_whisper_cpp_root();
std::filesystem::path get_whisper_model_path();
std::filesystem::path get_llama_cpp_root();
std::filesystem::path get_llama_model_path();
bool is_correction_enabled();
double get_correction_temperature();
int get_correction_top_k();
double get_correction_top_p();
double get_correction_min_p();
int get_correction_max_output_tokens();
int get_correction_segment_max_chars();
int get_correction_segment_overlap_chars();
int get_correction_force_segmentation_threshold_chars();
std::string get_correction_mode();
std::string describe_paths_json();
