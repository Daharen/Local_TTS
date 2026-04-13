#include "whisper_runner.h"

#include "paths.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

namespace {

std::filesystem::path find_whisper_cli_path(const std::filesystem::path& whisper_cpp_root) {
    const std::vector<std::filesystem::path> candidates = {
        whisper_cpp_root / "build" / "bin" / "Release" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "bin" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "Release" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "whisper-cli.exe",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

#ifdef _WIN32
std::wstring to_wide(const std::filesystem::path& p) {
    return p.wstring();
}
#endif

}  // namespace

int run_whisper_file_transcription(const std::filesystem::path& audio_path) {
    if (!std::filesystem::exists(audio_path)) {
        std::cerr << "Audio file not found: " << audio_path << '\n';
        return 1;
    }

    const auto model_path = get_whisper_model_path();
    if (!std::filesystem::exists(model_path)) {
        std::cerr << "Whisper model not found: " << model_path << '\n';
        return 1;
    }

    const auto whisper_cpp_root = get_whisper_cpp_root();
    const auto whisper_cli = find_whisper_cli_path(whisper_cpp_root);
    if (whisper_cli.empty()) {
        std::cerr << "whisper-cli.exe not found under: " << whisper_cpp_root << '\n';
        return 1;
    }

#ifdef _WIN32
    const auto exe_w = to_wide(whisper_cli);
    const auto model_w = to_wide(model_path);
    const auto audio_w = to_wide(audio_path);
    const wchar_t* args[] = {
        exe_w.c_str(),
        L"-m",
        model_w.c_str(),
        L"-f",
        audio_w.c_str(),
        L"-nt",
        nullptr,
    };

    const int rc = _wspawnv(_P_WAIT, exe_w.c_str(), args);
    if (rc == -1) {
        std::cerr << "Failed to launch whisper-cli.exe\n";
        return 1;
    }
    return rc;
#else
    const std::string cmd =
        "\"" + whisper_cli.string() + "\" -m \"" + model_path.string() + "\" -f \"" + audio_path.string() + "\" -nt";
    return std::system(cmd.c_str());
#endif
}
