#include <filesystem>
#include <iostream>
#include <string>

#include "live_mode.h"
#include "llm_correction.h"
#include "paths.h"
#include "whisper_runner.h"

namespace {

std::string join_args(int argc, char** argv, int start_index) {
    std::string out;
    for (int i = start_index; i < argc; ++i) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += argv[i];
    }
    return out;
}

void print_usage() {
    std::cerr << "Usage:\n"
              << "  local_tts\n"
              << "  local_tts transcribe <path-to-audio.wav>\n"
              << "  local_tts live\n"
              << "  local_tts live-debug\n"
              << "  local_tts llm-test <text>\n"
              << "  local_tts whisper-test <path-to-audio.wav>\n";
}


int run_whisper_test_command(const std::filesystem::path& audio_path) {
    std::string text;
    std::string error;
    WhisperRunInfo info;
    const bool ok = transcribe_file_to_string_with_info(audio_path, text, error, &info);

    std::cout << "[WHISPER_EXE] " << info.resolved_whisper_executable << "\n";
    std::cout << "[WHISPER_MODEL] " << info.resolved_model_path << "\n";
    std::cout << "[WHISPER_ARGS] " << info.argument_excerpt << "\n";
    std::cout << "[WHISPER_GPU_REQUESTED] " << (info.gpu_requested ? "true" : "false") << "\n";
    std::cout << "[WHISPER_GPU_ACTIVE] " << (info.gpu_active ? "true" : "false") << "\n";
    std::cout << "[WHISPER_CPU_FALLBACK] " << (info.cpu_fallback_reported ? "true" : "false") << "\n";
    std::cout << "[WHISPER_BACKEND_SUMMARY] " << info.backend_summary << "\n";
    if (!info.timing_excerpt.empty()) {
        std::cout << "[WHISPER_TIMING_EXCERPT] " << info.timing_excerpt << "\n";
    }
    if (!info.stderr_excerpt.empty()) {
        std::cout << "[WHISPER_STDERR_EXCERPT] " << info.stderr_excerpt << "\n";
    }
    if (!info.stdout_excerpt.empty()) {
        std::cout << "[WHISPER_STDOUT_EXCERPT] " << info.stdout_excerpt << "\n";
    }

    if (!ok) {
        if (!error.empty()) {
            std::cerr << error << "\n";
        }
        return 1;
    }

    if (!text.empty()) {
        std::cout << "[TRANSCRIPT] " << text << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto finalize = [](int code) {
        shutdown_llm_correction_backend();
        return code;
    };

    if (argc == 1) {
        std::cout << describe_paths_json() << '\n';
        return finalize(0);
    }

    if (argc == 3 && std::string(argv[1]) == "transcribe") {
        return finalize(run_whisper_file_transcription(argv[2]));
    }

    if (argc == 2 && std::string(argv[1]) == "live") {
        return finalize(run_live_mode(false));
    }

    if (argc == 2 && std::string(argv[1]) == "live-debug") {
        return finalize(run_live_mode(true));
    }

    if (argc >= 3 && std::string(argv[1]) == "llm-test") {
        return finalize(run_llm_test_command(join_args(argc, argv, 2)));
    }

    if (argc == 3 && std::string(argv[1]) == "whisper-test") {
        return finalize(run_whisper_test_command(argv[2]));
    }

    print_usage();
    return finalize(1);
}
