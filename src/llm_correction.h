#pragma once

#include <filesystem>
#include <string>

struct CorrectionRunInfo {
    std::filesystem::path llama_exe;
    std::filesystem::path llama_model;
    std::string prompt_mode;
};

bool correct_transcript_text(const std::string& raw_text, std::string& corrected_text, std::string& error_out);
bool correct_transcript_text_with_info(const std::string& raw_text,
                                       std::string& corrected_text,
                                       std::string& error_out,
                                       CorrectionRunInfo* info_out);
int run_llm_test_command(const std::string& input_text);
