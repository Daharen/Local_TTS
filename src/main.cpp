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
              << "  local_tts llm-test <text>\n";
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

    print_usage();
    return finalize(1);
}
