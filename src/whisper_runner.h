#pragma once

#include <filesystem>
#include <string>

int run_whisper_file_transcription(const std::filesystem::path& audio_path);
bool transcribe_file_to_string(const std::filesystem::path& audio_path, std::string& text_out, std::string& error_out);
