#include "whisper_runner.h"

#include "paths.h"
#include "pipeline_debug.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
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

std::string trim_copy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

#ifdef _WIN32
std::wstring to_wide(const std::filesystem::path& p) {
    return p.wstring();
}

std::wstring quote_arg(const std::wstring& arg) {
    std::wstring out = L"\"";
    out += arg;
    out += L"\"";
    return out;
}

bool read_handle_all(HANDLE handle, std::string& out) {
    if (!handle) {
        return false;
    }
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        out.append(buffer, buffer + bytes_read);
    }
    const DWORD last = GetLastError();
    return last == ERROR_BROKEN_PIPE || last == ERROR_SUCCESS;
}
#endif

bool resolve_transcribe_inputs(
    const std::filesystem::path& audio_path,
    std::filesystem::path& whisper_cli,
    std::filesystem::path& model_path,
    std::string& error_out) {
    if (!std::filesystem::exists(audio_path)) {
        error_out = "Audio file not found: " + audio_path.string();
        return false;
    }

    model_path = get_whisper_model_path();
    if (!std::filesystem::exists(model_path)) {
        error_out = "Whisper model not found: " + model_path.string();
        return false;
    }

    const auto whisper_cpp_root = get_whisper_cpp_root();
    whisper_cli = find_whisper_cli_path(whisper_cpp_root);
    if (whisper_cli.empty()) {
        error_out = "whisper-cli.exe not found under: " + whisper_cpp_root.string();
        return false;
    }

    return true;
}

}  // namespace

bool transcribe_file_to_string(const std::filesystem::path& audio_path, std::string& text_out, std::string& error_out) {
    text_out.clear();
    error_out.clear();
    pipeline_debug::log("whisper", "transcription requested for: " + audio_path.string());

    std::filesystem::path whisper_cli;
    std::filesystem::path model_path;
    if (!resolve_transcribe_inputs(audio_path, whisper_cli, model_path, error_out)) {
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) || !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        error_out = "Failed to create stdout pipe.";
        return false;
    }
    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0) || !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        error_out = "Failed to create stderr pipe.";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    const std::wstring cmd = quote_arg(to_wide(whisper_cli)) + L" -m " + quote_arg(to_wide(model_path)) + L" -f " + quote_arg(audio_path.wstring()) + L" -nt";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    const BOOL launched = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!launched) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        error_out = "Failed to launch whisper-cli.exe.";
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    read_handle_all(stdout_read, text_out);
    read_handle_all(stderr_read, error_out);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    text_out = trim_copy(text_out);
    error_out = trim_copy(error_out);
    if (exit_code == 0) {
        pipeline_debug::log("whisper", "transcription completed successfully");
    } else {
        pipeline_debug::log("whisper", error_out.empty() ? "whisper-cli exited with non-zero status" : error_out, true);
    }
    return exit_code == 0;
#else
    const std::string cmd =
        "\"" + whisper_cli.string() + "\" -m \"" + model_path.string() + "\" -f \"" + audio_path.string() + "\" -nt";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        error_out = "Failed to launch whisper-cli.";
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        text_out += buffer;
    }

    const int rc = pclose(pipe);
    text_out = trim_copy(text_out);
    if (rc != 0) {
        error_out = "whisper-cli failed.";
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }
    pipeline_debug::log("whisper", "transcription completed successfully");
    return true;
#endif
}

int run_whisper_file_transcription(const std::filesystem::path& audio_path) {
    std::string text;
    std::string error;
    if (!transcribe_file_to_string(audio_path, text, error)) {
        if (!error.empty()) {
            std::cerr << error << '\n';
        }
        return 1;
    }

    if (!text.empty()) {
        std::cout << text << '\n';
    }
    return 0;
}
