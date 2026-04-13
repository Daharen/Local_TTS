#pragma once

#include <filesystem>
#include <string>

std::filesystem::path get_repo_root();
std::filesystem::path get_large_data_root();
std::string describe_paths_json();
