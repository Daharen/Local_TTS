#pragma once

#include <filesystem>
#include <string>

std::filesystem::path get_repo_root();
std::filesystem::path get_large_data_root();
std::filesystem::path get_whisper_cpp_root();
std::filesystem::path get_whisper_model_path();
std::string describe_paths_json();
