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
std::string describe_paths_json();
