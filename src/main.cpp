#include <iostream>
#include <string>

#include "live_mode.h"
#include "paths.h"
#include "whisper_runner.h"

namespace {

void print_usage() {
    std::cerr << "Usage:\n"
              << "  local_tts\n"
              << "  local_tts transcribe <path-to-audio.wav>\n"
              << "  local_tts live\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        std::cout << describe_paths_json() << '\n';
        return 0;
    }

    if (argc == 3 && std::string(argv[1]) == "transcribe") {
        return run_whisper_file_transcription(argv[2]);
    }

    if (argc == 2 && std::string(argv[1]) == "live") {
        return run_live_mode();
    }

    print_usage();
    return 1;
}
