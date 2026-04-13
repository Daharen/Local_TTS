#include "llm_correction.h"

#include "paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string trim_copy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string normalized_correction_mode() {
    std::string mode = trim_copy(get_correction_mode());
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode == "notes") {
        return "notes";
    }
    return "formatted";
}

std::size_t non_space_count(const std::string& s) {
    return static_cast<std::size_t>(
        std::count_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
}

bool is_trivial_cleanup(const std::string& raw, const std::string& corrected) {
    if (raw.empty() || corrected.empty()) {
        return false;
    }
    const std::string a = trim_copy(raw);
    const std::string b = trim_copy(corrected);
    if (a == b) {
        return true;
    }
    if (!a.empty() && !b.empty()) {
        const bool same_ignoring_trailing_period = (a + "." == b) || (b + "." == a);
        if (same_ignoring_trailing_period) {
            return true;
        }
    }
    return false;
}

bool looks_like_meta_output(const std::string& text) {
    const std::string t = trim_copy(text);
    if (t.empty()) {
        return true;
    }

    std::string lower;
    lower.reserve(t.size());
    for (unsigned char ch : t) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }

    return lower.rfind("here", 0) == 0 || lower.rfind("corrected", 0) == 0 || lower.rfind("output:", 0) == 0 ||
           lower.rfind("the corrected", 0) == 0;
}

std::filesystem::path find_llama_cli_path(const std::filesystem::path& llama_root) {
    const std::vector<std::filesystem::path> candidates = {
        llama_root / "build" / "bin" / "Release" / "llama-cli.exe",
        llama_root / "build" / "bin" / "llama-cli.exe",
        llama_root / "build" / "Release" / "llama-cli.exe",
        llama_root / "build" / "llama-cli.exe",
        llama_root / "build" / "bin" / "Release" / "main.exe",
        llama_root / "build" / "bin" / "main.exe",
        llama_root / "build" / "Release" / "main.exe",
        llama_root / "build" / "main.exe",
    };

    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return {};
}

bool resolve_llama_inputs(std::filesystem::path& llama_exe, std::filesystem::path& llama_model, std::string& error_out) {
    llama_exe.clear();
    llama_model.clear();
    error_out.clear();

    const auto llama_cpp_root = get_llama_cpp_root();
    llama_model = get_llama_model_path();
    llama_exe = find_llama_cli_path(llama_cpp_root);

    if (llama_exe.empty()) {
        error_out = "llama executable not found under: " + llama_cpp_root.string();
        return false;
    }
    if (!std::filesystem::exists(llama_model)) {
        error_out = "llama model not found: " + llama_model.string();
        return false;
    }
    return true;
}

std::string build_system_prompt() {
    const std::string mode = normalized_correction_mode();
    if (mode == "notes") {
        return "You are an AI assistant turning speech-to-text transcription into clean readable notes.\n\n"
               "Task:\n"
               "- Fix grammar, punctuation, capitalization, and spacing.\n"
               "- Remove filler words, filled pauses, and hesitation artifacts.\n"
               "- Preserve meaning.\n"
               "- Improve readability with paragraph breaks and line breaks.\n"
               "- Use bullets, indentation, or list structure when the spoken content clearly supports it.\n"
               "- Avoid giant monotonous blocks of text.\n"
               "- Do not invent content.\n"
               "- Do not explain anything.\n"
               "- Output only the final formatted notes.";
    }

    return "You are an AI assistant cleaning and formatting speech-to-text transcription output.\n\n"
           "Task:\n"
           "- Fix grammar, punctuation, capitalization, and spacing errors.\n"
           "- Remove filler words, filled pauses, and hesitation artifacts.\n"
           "- Correct obvious transcription mistakes conservatively.\n"
           "- Preserve the intended meaning.\n"
           "- Improve readability with paragraph breaks and line breaks.\n"
           "- Use indentation or light structure only when clearly warranted by the content.\n"
           "- Avoid giant monotonous blocks of text.\n"
           "- Do not add new content.\n"
           "- Do not explain anything.\n"
           "- Output only the final formatted text.\n\n"
           "Correction-speech rule:\n"
           "If the speaker corrects themselves, keep only the final intended correction.\n"
           "Example:\n"
           "Input: \"lets meet at 3PM, oh i mean, 4PM\"\n"
           "Output: \"Let's meet at 4 PM.\"\n\n"
           "Ambiguity rule:\n"
           "If the text is ambiguous, fragmented, or multiple interpretations are plausible, preserve the original "
           "wording as much as possible rather than inventing a new meaning.";
}

std::string build_user_prompt(const std::string& raw_text) {
    return "Transcript:\n" + raw_text;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

template <typename T>
std::wstring number_to_wide(T value) {
    return utf8_to_wide(std::to_string(value));
}

std::wstring quote_windows_arg(const std::wstring& arg) {
    std::wstring out;
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }
    if (backslashes > 0) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(const std::vector<std::wstring>& args) {
    std::wstring out;
    bool first = true;
    for (const auto& arg : args) {
        if (!first) {
            out.push_back(L' ');
        }
        first = false;
        out += quote_windows_arg(arg);
    }
    return out;
}

std::string first_line(const std::string& text) {
    const std::string trimmed = trim_copy(text);
    const auto pos = trimmed.find_first_of("\r\n");
    return trim_copy(pos == std::string::npos ? trimmed : trimmed.substr(0, pos));
}

bool read_handle_all(HANDLE handle, std::string& out) {
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        out.append(buffer, buffer + bytes_read);
    }
    const DWORD last = GetLastError();
    return last == ERROR_BROKEN_PIPE || last == ERROR_SUCCESS;
}

bool run_llama_cli(const std::filesystem::path& exe,
                   const std::filesystem::path& model,
                   const std::string& system_prompt,
                   const std::string& user_prompt,
                   std::string& stdout_out,
                   std::string& stderr_out,
                   std::string& error_out,
                   bool with_reasoning_flag) {
    stdout_out.clear();
    stderr_out.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_null = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) || !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        error_out = "Failed to create llama stdout pipe.";
        return false;
    }

    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0) || !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        error_out = "Failed to create llama stderr pipe.";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    stdin_null = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = (stdin_null != INVALID_HANDLE_VALUE) ? stdin_null : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::vector<std::wstring> args = {exe.wstring(),
                                      L"-m",
                                      model.wstring(),
                                      L"--temp",
                                      number_to_wide(get_correction_temperature()),
                                      L"--top-k",
                                      number_to_wide(get_correction_top_k()),
                                      L"--top-p",
                                      number_to_wide(get_correction_top_p()),
                                      L"--min-p",
                                      number_to_wide(get_correction_min_p()),
                                      L"--single-turn",
                                      L"--no-display-prompt",
                                      L"--simple-io",
                                      L"-n",
                                      L"128",
                                      L"-sys",
                                      utf8_to_wide(system_prompt),
                                      L"-p",
                                      utf8_to_wide(user_prompt)};
    if (with_reasoning_flag) {
        args.push_back(L"--reasoning");
        args.push_back(L"off");
    }

    std::wstring cmd_string = build_command_line(args);
    std::vector<wchar_t> cmdline(cmd_string.begin(), cmd_string.end());
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
    if (stdin_null != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_null);
    }

    if (!launched) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        error_out = "Failed to launch llama-cli executable.";
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    read_handle_all(stdout_read, stdout_out);
    read_handle_all(stderr_read, stderr_out);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0) {
        error_out = first_line(stderr_out);
        if (error_out.empty()) {
            error_out = first_line(stdout_out);
        }
        if (error_out.empty()) {
            error_out = "llama-cli failed with non-zero exit code: " + std::to_string(static_cast<unsigned long>(exit_code));
        }
        return false;
    }

    return true;
}
#endif

}  // namespace

bool correct_transcript_text_with_info(const std::string& raw_text,
                                       std::string& corrected_text,
                                       std::string& error_out,
                                       CorrectionRunInfo* info_out) {
    corrected_text.clear();
    error_out.clear();
    if (info_out) {
        *info_out = {};
        info_out->correction_mode = normalized_correction_mode();
    }

    const std::string raw_trimmed = trim_copy(raw_text);
    if (raw_trimmed.empty()) {
        error_out = "Raw transcript is empty.";
        return false;
    }

#ifdef _WIN32
    std::filesystem::path llama_exe;
    std::filesystem::path llama_model;
    if (!resolve_llama_inputs(llama_exe, llama_model, error_out)) {
        return false;
    }
    if (info_out) {
        info_out->llama_exe = llama_exe;
        info_out->llama_model = llama_model;
    }

    std::string stdout_text;
    std::string stderr_text;
    const std::string system_prompt = build_system_prompt();
    const std::string user_prompt = build_user_prompt(raw_trimmed);
    if (!run_llama_cli(
            llama_exe, llama_model, system_prompt, user_prompt, stdout_text, stderr_text, error_out, true) &&
        !run_llama_cli(llama_exe, llama_model, system_prompt, user_prompt, stdout_text, stderr_text, error_out, false)) {
        return false;
    }

    corrected_text = trim_copy(stdout_text);
    if (corrected_text.empty() || looks_like_meta_output(corrected_text)) {
        error_out = "Correction output was empty or non-transcript.";
        return false;
    }

    const bool is_short = non_space_count(raw_trimmed) < 8;
    if (is_short && !is_trivial_cleanup(raw_trimmed, corrected_text)) {
        corrected_text = raw_trimmed;
        return true;
    }

    return true;
#else
    error_out = "LLM correction is only supported on Windows.";
    return false;
#endif
}

bool correct_transcript_text(const std::string& raw_text, std::string& corrected_text, std::string& error_out) {
    return correct_transcript_text_with_info(raw_text, corrected_text, error_out, nullptr);
}

int run_llm_test_command(const std::string& input_text) {
    std::cout << "[LLM_TEST_INPUT] " << input_text << "\n";

    CorrectionRunInfo info;
    std::string corrected;
    std::string error;
    const bool ok = correct_transcript_text_with_info(input_text, corrected, error, &info);

    std::cout << "[LLM_TEST_MODEL] " << info.llama_model.string() << "\n";
    std::cout << "[LLM_TEST_EXE] " << info.llama_exe.string() << "\n";
    std::cout << "[LLM_TEST_MODE] " << normalized_correction_mode() << "\n";

    if (!ok) {
        std::cout << "[LLM_TEST_ERROR] " << error << "\n";
        return 1;
    }

    std::cout << "[LLM_TEST_OUTPUT] " << corrected << "\n";
    std::cout << "[LLM_TEST_ERROR] " << "\n";
    return 0;
}
