#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct CorrectionRunInfo {
    std::filesystem::path llama_exe;
    std::filesystem::path llama_model;
    std::string correction_mode;
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
int run_llm_test_command(const std::string& input_text);
