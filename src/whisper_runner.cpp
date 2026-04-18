#include "whisper_runner.h"

#include "paths.h"
#include "pipeline_debug.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#endif

namespace {

struct WhisperProfile {
    std::string name;
    std::vector<std::string> args;
    bool requests_gpu = false;
};

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

std::string trim_copy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string to_lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string clip_excerpt(const std::string& text, std::size_t limit = 320) {
    const auto trimmed = trim_copy(text);
    if (trimmed.size() <= limit) {
        return trimmed;
    }
    if (limit < 4) {
        return trimmed.substr(0, limit);
    }
    return trimmed.substr(0, limit - 3) + "...";
}

std::filesystem::path find_whisper_cli_path(const std::filesystem::path& whisper_cpp_root) {
    const std::vector<std::filesystem::path> candidates = {
        whisper_cpp_root / "build" / "bin" / "Release" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "bin" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "Release" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "whisper-cli.exe",
        whisper_cpp_root / "build" / "bin" / "Release" / "whisper-cli",
        whisper_cpp_root / "build" / "bin" / "whisper-cli",
        whisper_cpp_root / "build" / "whisper-cli",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

std::string quote_shell(const std::string& arg) {
#ifdef _WIN32
    return std::string("\"") + arg + "\"";
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
#endif
}

std::string build_command_line(const std::filesystem::path& exe_path, const std::vector<std::string>& args) {
    std::ostringstream out;
    out << quote_shell(exe_path.string());
    for (const auto& arg : args) {
        out << ' ' << quote_shell(arg);
    }
    return out.str();
}

#ifdef _WIN32
std::wstring quote_windows_arg(const std::wstring& arg) {
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

ProcessResult run_process_capture(const std::filesystem::path& exe_path, const std::vector<std::string>& args) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) || !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        result.stderr_text = "Failed to create stdout pipe.";
        return result;
    }
    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0) || !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        result.stderr_text = "Failed to create stderr pipe.";
        return result;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = quote_windows_arg(exe_path.wstring());
    for (const auto& arg : args) {
        cmd += L" ";
        cmd += quote_windows_arg(std::filesystem::path(arg).wstring());
    }

    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    const BOOL launched = CreateProcessW(nullptr,
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
        result.stderr_text = "Failed to launch whisper-cli executable.";
        return result;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    read_handle_all(stdout_read, result.stdout_text);
    read_handle_all(stderr_read, result.stderr_text);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}
#else
ProcessResult run_process_capture(const std::filesystem::path& exe_path, const std::vector<std::string>& args) {
    ProcessResult result;
    const std::string cmd = build_command_line(exe_path, args) + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.stderr_text = "Failed to launch whisper-cli executable.";
        return result;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result.stdout_text += buffer;
    }

    const int rc = pclose(pipe);
    result.exit_code = rc;
    return result;
}
#endif

std::vector<WhisperProfile> build_profiles(const std::filesystem::path& model_path, const std::filesystem::path& audio_path) {
    const bool request_gpu = is_whisper_gpu_requested();
    const int gpu_device = get_whisper_gpu_device();
    const bool flash_attn = is_whisper_flash_attn_enabled();
    const int threads = get_whisper_threads();

    std::vector<std::string> base_args = {
        "-m", model_path.string(),
        "-f", audio_path.string(),
        "-nt",
    };

    if (threads > 0) {
        base_args.push_back("-t");
        base_args.push_back(std::to_string(threads));
    }

    std::vector<WhisperProfile> profiles;
    if (request_gpu) {
        WhisperProfile cuda_device{"gpu_device_cuda", base_args, true};
        cuda_device.args.insert(cuda_device.args.end(), {"--device", "cuda", "--gpu-device", std::to_string(gpu_device)});
        if (flash_attn) {
            cuda_device.args.push_back("--flash-attn");
        }
        profiles.push_back(cuda_device);

        WhisperProfile gpu_device_only{"gpu_device_only", base_args, true};
        gpu_device_only.args.insert(gpu_device_only.args.end(), {"--gpu-device", std::to_string(gpu_device)});
        if (flash_attn) {
            gpu_device_only.args.push_back("--flash-attn");
        }
        profiles.push_back(gpu_device_only);
    }

    profiles.push_back(WhisperProfile{"cpu_safe", base_args, false});
    return profiles;
}

bool should_try_next_profile(const ProcessResult& result, bool gpu_profile) {
    if (!gpu_profile) {
        return false;
    }

    const std::string combined = to_lower_copy(result.stdout_text + "\n" + result.stderr_text);
    if (result.exit_code != 0) {
        return true;
    }

    return combined.find("unknown argument") != std::string::npos ||
           combined.find("invalid argument") != std::string::npos ||
           combined.find("no gpu found") != std::string::npos ||
           combined.find("gpu backend") != std::string::npos ||
           combined.find("failed to initialize") != std::string::npos;
}

void classify_backend(const ProcessResult& result, bool gpu_requested, WhisperRunInfo& info) {
    const std::string combined = to_lower_copy(result.stdout_text + "\n" + result.stderr_text);

    const bool explicit_no_gpu = combined.find("no gpu found") != std::string::npos ||
                                 combined.find("backend_init_gpu") != std::string::npos ||
                                 combined.find("gpu backend") != std::string::npos;

    const bool has_gpu_signal = combined.find("cuda") != std::string::npos ||
                                combined.find("cublas") != std::string::npos ||
                                combined.find("vulkan") != std::string::npos ||
                                combined.find("metal") != std::string::npos;

    const bool cpu_signal = combined.find("device 0: cpu") != std::string::npos ||
                            combined.find("using cpu") != std::string::npos;

    info.gpu_active = has_gpu_signal && !explicit_no_gpu;
    info.cpu_fallback_reported = cpu_signal || (gpu_requested && explicit_no_gpu);

    if (info.gpu_active) {
        info.backend_summary = gpu_requested ? "gpu active" : "gpu active (auto)";
    } else if (gpu_requested && explicit_no_gpu) {
        info.backend_summary = "gpu requested but unavailable; cpu fallback";
    } else if (cpu_signal || !gpu_requested) {
        info.backend_summary = "cpu";
    } else {
        info.backend_summary = "unknown/unclassified";
    }

    std::istringstream lines(result.stderr_text + "\n" + result.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto lower = to_lower_copy(line);
        if (lower.find("time") != std::string::npos || lower.find("timings") != std::string::npos) {
            info.timing_excerpt = clip_excerpt(line, 220);
            break;
        }
    }
}

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

    const auto cli_override = get_whisper_cli_path_override();
    if (!cli_override.empty()) {
        if (!std::filesystem::exists(cli_override)) {
            error_out = "Configured whisper executable not found: " + cli_override.string();
            return false;
        }
        whisper_cli = cli_override;
        return true;
    }

    const auto whisper_cpp_root = get_whisper_cpp_root();
    whisper_cli = find_whisper_cli_path(whisper_cpp_root);
    if (whisper_cli.empty()) {
        error_out = "whisper-cli executable not found under: " + whisper_cpp_root.string();
        return false;
    }

    return true;
}

std::string join_args_excerpt(const std::vector<std::string>& args, std::size_t max_len = 280) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += arg;
    }
    return clip_excerpt(out, max_len);
}

}  // namespace

bool transcribe_file_to_string_with_info(
    const std::filesystem::path& audio_path,
    std::string& text_out,
    std::string& error_out,
    WhisperRunInfo* info_out) {
    text_out.clear();
    error_out.clear();

    WhisperRunInfo local_info{};
    WhisperRunInfo& info = info_out ? *info_out : local_info;
    info = WhisperRunInfo{};
    info.gpu_requested = is_whisper_gpu_requested();

    std::filesystem::path whisper_cli;
    std::filesystem::path model_path;
    if (!resolve_transcribe_inputs(audio_path, whisper_cli, model_path, error_out)) {
        info.resolved_whisper_executable = whisper_cli.string();
        info.resolved_model_path = model_path.string();
        info.backend_summary = "resolve-failed";
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }

    info.resolved_whisper_executable = whisper_cli.string();
    info.resolved_model_path = model_path.string();

    const auto profiles = build_profiles(model_path, audio_path);
    std::string profile_notes;

    ProcessResult chosen_result;
    std::vector<std::string> chosen_args;
    bool profile_selected = false;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto& profile = profiles[i];
        const ProcessResult result = run_process_capture(whisper_cli, profile.args);

        if (should_try_next_profile(result, profile.requests_gpu) && i + 1 < profiles.size()) {
            profile_notes += "profile=" + profile.name + " rejected; ";
            continue;
        }

        profile_selected = true;
        chosen_result = result;
        chosen_args = profile.args;
        profile_notes += "profile=" + profile.name + " selected";
        break;
    }

    if (!profile_selected) {
        error_out = "Unable to execute whisper profile.";
        info.backend_summary = "launch-failed";
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }

    text_out = trim_copy(chosen_result.stdout_text);
    error_out = trim_copy(chosen_result.stderr_text);

    info.argument_excerpt = join_args_excerpt(chosen_args);
    info.stdout_excerpt = clip_excerpt(chosen_result.stdout_text);
    info.stderr_excerpt = clip_excerpt(chosen_result.stderr_text);
    info.exit_code = chosen_result.exit_code;

    classify_backend(chosen_result, info.gpu_requested, info);
    pipeline_debug::log("whisper", profile_notes + ", backend=" + info.backend_summary);

    if (chosen_result.exit_code != 0) {
        if (error_out.empty()) {
            error_out = "whisper-cli exited with non-zero status.";
        }
        pipeline_debug::log("whisper", error_out, true);
        return false;
    }

    return true;
}

bool transcribe_file_to_string(const std::filesystem::path& audio_path, std::string& text_out, std::string& error_out) {
    return transcribe_file_to_string_with_info(audio_path, text_out, error_out, nullptr);
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
