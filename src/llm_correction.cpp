#include "llm_correction.h"

#include "paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

std::string normalize_newlines(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;
            }
            out.push_back('\n');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string normalize_join_whitespace(std::string text) {
    text = normalize_newlines(text);
    std::string out;
    out.reserve(text.size());
    bool last_space = false;
    int newline_run = 0;
    for (unsigned char ch : text) {
        if (ch == '\n') {
            ++newline_run;
            last_space = false;
            continue;
        }
        if (newline_run > 0) {
            if (!out.empty()) {
                out += (newline_run >= 2) ? "\n\n" : "\n";
            }
            newline_run = 0;
        }
        if (std::isspace(ch)) {
            if (!last_space && !out.empty() && out.back() != '\n') {
                out.push_back(' ');
            }
            last_space = true;
            continue;
        }
        out.push_back(static_cast<char>(ch));
        last_space = false;
    }
    return trim_copy(out);
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

enum class LlamaFrontend {
    Completion,
    Cli,
    Main,
    Unknown,
};

LlamaFrontend detect_llama_frontend(const std::filesystem::path& exe_path) {
    std::string name = exe_path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.find("llama-completion") != std::string::npos) {
        return LlamaFrontend::Completion;
    }
    if (name.find("llama-cli") != std::string::npos) {
        return LlamaFrontend::Cli;
    }
    if (name == "main.exe" || name == "main") {
        return LlamaFrontend::Main;
    }
    return LlamaFrontend::Unknown;
}

std::filesystem::path find_llama_executable_path(const std::filesystem::path& llama_root) {
    const std::vector<std::filesystem::path> candidates = {
        llama_root / "build" / "bin" / "Release" / "llama-completion.exe",
        llama_root / "build" / "bin" / "llama-completion.exe",
        llama_root / "build" / "Release" / "llama-completion.exe",
        llama_root / "build" / "llama-completion.exe",
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
    llama_exe = find_llama_executable_path(llama_cpp_root);

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
    return "Rewrite this speech-to-text transcript into clean final text.\n"
           "Only output the rewritten text.\n"
           "Do not include labels, explanations, prompts, or metadata.\n\n"
           "Transcript:\n" +
           raw_text;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string to_lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::stringstream stream(text);
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string collapse_space_copy(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!in_space) {
                out.push_back(' ');
                in_space = true;
            }
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(ch)));
        in_space = false;
    }
    return trim_copy(out);
}

bool is_banner_line(const std::string& line_lower) {
    return starts_with(line_lower, "loading model") || starts_with(line_lower, "build") || starts_with(line_lower, "model") ||
           starts_with(line_lower, "modalities") || starts_with(line_lower, "using custom system prompt") ||
           starts_with(line_lower, "ggml_") || starts_with(line_lower, "main:") || starts_with(line_lower, "system info");
}

bool is_shell_help_line(const std::string& line, const std::string& line_lower) {
    return starts_with(line, "/") || starts_with(line_lower, "available commands") ||
           starts_with(line_lower, "write '/' to prefix commands") || starts_with(line_lower, "common params:") ||
           starts_with(line_lower, "sampling params:") || starts_with(line_lower, "chat params:") ||
           starts_with(line_lower, "server listening on") || starts_with(line_lower, "ctrl+c");
}

bool is_footer_line(const std::string& line, const std::string& line_lower) {
    if (starts_with(line_lower, "exiting")) {
        return true;
    }
    if (starts_with(line, "[") && !line.empty() && line.back() == ']') {
        const std::string lower = line_lower;
        return lower.find("prompt:") != std::string::npos || lower.find("generation:") != std::string::npos ||
               lower.find("tokens/s") != std::string::npos || lower.find("tok/s") != std::string::npos ||
               lower.find("time") != std::string::npos;
    }
    return false;
}

bool is_prompt_echo_line(const std::string& line, const std::string& line_lower) {
    return starts_with(line, ">") || line_lower == "transcript:" || line_lower == "assistant:" || line_lower == "user:" ||
           line_lower == "output:" || line_lower == "response:" ||
           line_lower == "rewrite this speech-to-text transcript into clean final text." ||
           line_lower == "only output the rewritten text." ||
           line_lower == "do not include labels, explanations, prompts, or metadata.";
}

bool is_raw_transcript_echo_line(const std::string& line_norm, const std::string& raw_norm, const std::string& raw_single_line_norm) {
    if (line_norm.empty() || raw_norm.empty()) {
        return false;
    }
    return line_norm == raw_norm || line_norm == raw_single_line_norm ||
           (!line_norm.empty() && raw_norm.find(line_norm) != std::string::npos);
}

bool has_meaningful_text(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) { return std::isalnum(ch); });
}

bool is_candidate_payload_line(const std::string& line,
                               const std::string& line_lower,
                               const std::string& line_norm,
                               const std::string& raw_norm,
                               const std::string& raw_single_line_norm) {
    if (line.empty()) {
        return false;
    }
    if (is_banner_line(line_lower) || is_shell_help_line(line, line_lower) || is_footer_line(line, line_lower) ||
        is_prompt_echo_line(line, line_lower) || is_raw_transcript_echo_line(line_norm, raw_norm, raw_single_line_norm)) {
        return false;
    }
    return true;
}

bool block_matches_raw_transcript(const std::string& block, const std::string& raw_trimmed) {
    const std::string block_norm = collapse_space_copy(normalize_newlines(block));
    const std::string raw_norm = collapse_space_copy(normalize_newlines(raw_trimmed));
    return !block_norm.empty() && block_norm == raw_norm;
}

std::string sanitize_llama_stdout(const std::string& raw_stdout, const std::string& raw_trimmed) {
    const std::string normalized = normalize_newlines(raw_stdout);
    const std::string raw_norm = collapse_space_copy(raw_trimmed);
    std::string raw_single_line = raw_trimmed;
    std::replace(raw_single_line.begin(), raw_single_line.end(), '\n', ' ');
    const std::string raw_single_line_norm = collapse_space_copy(raw_single_line);

    std::vector<std::string> candidate_lines;
    for (const std::string& raw_line : split_lines(normalized)) {
        const std::string line = trim_copy(raw_line);
        if (line.empty()) {
            if (!candidate_lines.empty() && !candidate_lines.back().empty()) {
                candidate_lines.emplace_back();
            }
            continue;
        }
        const std::string lower = to_lower_copy(line);
        const std::string line_norm = collapse_space_copy(line);
        if (is_candidate_payload_line(line, lower, line_norm, raw_norm, raw_single_line_norm)) {
            candidate_lines.push_back(line);
        }
    }

    while (!candidate_lines.empty() && candidate_lines.back().empty()) {
        candidate_lines.pop_back();
    }

    std::vector<std::string> blocks;
    std::string current;
    for (const auto& line : candidate_lines) {
        if (line.empty()) {
            if (!current.empty()) {
                blocks.push_back(trim_copy(current));
                current.clear();
            }
            continue;
        }
        if (!current.empty()) {
            current.push_back('\n');
        }
        current += line;
    }
    if (!current.empty()) {
        blocks.push_back(trim_copy(current));
    }

    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        const std::string block = trim_copy(*it);
        if (!has_meaningful_text(block)) {
            continue;
        }
        const std::string lower = to_lower_copy(block);
        if (is_prompt_echo_line(block, lower) || is_shell_help_line(block, lower) || is_footer_line(block, lower)) {
            continue;
        }
        if (block_matches_raw_transcript(block, raw_trimmed)) {
            continue;
        }
        return block;
    }

    return {};
}

std::string compact_debug_excerpt(const std::string& text, std::size_t limit = 320) {
    std::string normalized = normalize_newlines(text);
    normalized = trim_copy(normalized);
    if (normalized.size() <= limit) {
        return normalized;
    }
    return normalized.substr(0, limit) + "...(trimmed)";
}

int pick_split_pos(const std::string& text, int start, int max_chars) {
    const int end = (std::min)(static_cast<int>(text.size()), start + max_chars);
    if (end <= start) {
        return start;
    }
    if (end == static_cast<int>(text.size())) {
        return end;
    }

    int best = -1;
    for (int i = end; i > start + max_chars / 2; --i) {
        if (i >= 2 && text[static_cast<std::size_t>(i - 1)] == '\n' && text[static_cast<std::size_t>(i - 2)] == '\n') {
            return i;
        }
    }
    for (int i = end; i > start + max_chars / 2; --i) {
        const char c = text[static_cast<std::size_t>(i - 1)];
        if (c == '.' || c == '!' || c == '?') {
            best = i;
            break;
        }
    }
    if (best > 0) {
        return best;
    }
    for (int i = end; i > start + max_chars / 2; --i) {
        if (std::isspace(static_cast<unsigned char>(text[static_cast<std::size_t>(i - 1)]))) {
            return i;
        }
    }
    return end;
}

std::vector<std::string> build_segments(const std::string& raw, int max_chars, int overlap_chars) {
    std::vector<std::string> segments;
    if (raw.empty()) {
        return segments;
    }

    const int safe_max = (std::max)(200, max_chars);
    const int safe_overlap = (std::max)(0, (std::min)(overlap_chars, safe_max / 3));

    int start = 0;
    while (start < static_cast<int>(raw.size())) {
        while (start < static_cast<int>(raw.size()) && std::isspace(static_cast<unsigned char>(raw[static_cast<std::size_t>(start)]))) {
            ++start;
        }
        if (start >= static_cast<int>(raw.size())) {
            break;
        }

        const int split = pick_split_pos(raw, start, safe_max);
        int end = (std::max)(split, start + 1);
        while (end > start && std::isspace(static_cast<unsigned char>(raw[static_cast<std::size_t>(end - 1)]))) {
            --end;
        }
        if (end <= start) {
            end = (std::min)(start + safe_max, static_cast<int>(raw.size()));
        }

        segments.push_back(trim_copy(raw.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start))));
        if (end >= static_cast<int>(raw.size())) {
            break;
        }
        start = (std::max)(0, end - safe_overlap);
    }

    return segments;
}

std::string overlap_key(const std::string& text) {
    return collapse_space_copy(normalize_join_whitespace(text));
}

std::size_t find_overlap_cut(const std::string& merged, const std::string& next, int overlap_chars) {
    const std::string a = overlap_key(merged);
    const std::string b = overlap_key(next);
    if (a.empty() || b.empty()) {
        return 0;
    }

    const std::size_t max_scan = static_cast<std::size_t>((std::max)(64, overlap_chars * 2));
    const std::size_t max_len = (std::min)((std::min)(a.size(), b.size()), max_scan);
    std::size_t best = 0;
    for (std::size_t len = max_len; len >= 12; --len) {
        if (a.compare(a.size() - len, len, b, 0, len) == 0) {
            best = len;
            break;
        }
        if (len == 12) {
            break;
        }
    }

    if (best == 0) {
        return 0;
    }

    std::size_t consumed = 0;
    std::size_t chars = 0;
    while (consumed < b.size() && chars < best) {
        if (!std::isspace(static_cast<unsigned char>(b[consumed]))) {
            ++chars;
        }
        ++consumed;
    }
    return consumed;
}

std::string merge_segments(const std::vector<std::string>& parts, int overlap_chars) {
    std::string merged;
    for (const auto& part_raw : parts) {
        const std::string part = normalize_join_whitespace(part_raw);
        if (part.empty()) {
            continue;
        }
        if (merged.empty()) {
            merged = part;
            continue;
        }

        const std::size_t cut = find_overlap_cut(merged, part, overlap_chars);
        std::string tail = trim_copy(part.substr(cut));
        if (!tail.empty()) {
            if (!merged.empty() && merged.back() != '\n') {
                merged.push_back(' ');
            }
            merged += tail;
        }
    }
    return normalize_join_whitespace(merged);
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

bool run_llama_process(const std::filesystem::path& exe,
                       const std::filesystem::path& model,
                       LlamaFrontend frontend,
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
    std::vector<std::wstring> args = {exe.wstring(), L"-m", model.wstring()};

    const auto push_sampling_flags = [&]() {
        args.push_back(L"--temp");
        args.push_back(number_to_wide(get_correction_temperature()));
        args.push_back(L"--top-k");
        args.push_back(number_to_wide(get_correction_top_k()));
        args.push_back(L"--top-p");
        args.push_back(number_to_wide(get_correction_top_p()));
        args.push_back(L"--min-p");
        args.push_back(number_to_wide(get_correction_min_p()));
    };

    const std::wstring max_tokens = number_to_wide(get_correction_max_output_tokens());

    switch (frontend) {
        case LlamaFrontend::Completion:
            push_sampling_flags();
            args.push_back(L"-no-cnv");
            args.push_back(L"--single-turn");
            args.push_back(L"--no-display-prompt");
            args.push_back(L"--simple-io");
            args.push_back(L"--log-disable");
            args.push_back(L"--no-warmup");
            if (with_reasoning_flag) {
                args.push_back(L"--reasoning");
                args.push_back(L"off");
            }
            args.push_back(L"-n");
            args.push_back(max_tokens);
            args.push_back(L"-sys");
            args.push_back(utf8_to_wide(system_prompt));
            args.push_back(L"-p");
            args.push_back(utf8_to_wide(user_prompt));
            break;
        case LlamaFrontend::Cli:
            push_sampling_flags();
            args.push_back(L"--single-turn");
            args.push_back(L"--no-display-prompt");
            args.push_back(L"--simple-io");
            args.push_back(L"--log-disable");
            args.push_back(L"--no-warmup");
            if (with_reasoning_flag) {
                args.push_back(L"--reasoning");
                args.push_back(L"off");
            }
            args.push_back(L"-n");
            args.push_back(max_tokens);
            args.push_back(L"-sys");
            args.push_back(utf8_to_wide(system_prompt));
            args.push_back(L"-p");
            args.push_back(utf8_to_wide(user_prompt));
            break;
        case LlamaFrontend::Main:
        case LlamaFrontend::Unknown: {
            push_sampling_flags();
            args.push_back(L"--simple-io");
            args.push_back(L"--log-disable");
            args.push_back(L"--no-warmup");
            args.push_back(L"-n");
            args.push_back(max_tokens);
            const std::string merged_prompt =
                system_prompt + "\n\nUser transcript request:\n" + user_prompt + "\n\nOnly output rewritten text.";
            args.push_back(L"-p");
            args.push_back(utf8_to_wide(merged_prompt));
            break;
        }
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
        error_out = "Failed to launch llama executable.";
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
            error_out = "llama executable failed with non-zero exit code: " + std::to_string(static_cast<unsigned long>(exit_code));
        }
        return false;
    }

    return true;
}

bool run_single_correction(const std::filesystem::path& llama_exe,
                           const std::filesystem::path& llama_model,
                           const std::string& raw_trimmed,
                           std::string& corrected_text,
                           std::string& debug_stdout,
                           std::string& error_out) {
    std::string stdout_text;
    std::string stderr_text;
    const std::string system_prompt = build_system_prompt();
    const std::string user_prompt = build_user_prompt(raw_trimmed);
    const LlamaFrontend frontend = detect_llama_frontend(llama_exe);
    if (!run_llama_process(
            llama_exe, llama_model, frontend, system_prompt, user_prompt, stdout_text, stderr_text, error_out, true) &&
        !run_llama_process(
            llama_exe, llama_model, frontend, system_prompt, user_prompt, stdout_text, stderr_text, error_out, false)) {
        return false;
    }

    corrected_text = sanitize_llama_stdout(stdout_text, raw_trimmed);
    debug_stdout = compact_debug_excerpt(stdout_text);

    if (corrected_text.empty() || looks_like_meta_output(corrected_text)) {
        error_out = "Correction output was empty or unusable after sanitization.";
        return false;
    }

    if (block_matches_raw_transcript(corrected_text, raw_trimmed)) {
        error_out = "Correction output matched raw transcript.";
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
        info_out->max_output_tokens = get_correction_max_output_tokens();
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

    const int seg_threshold = get_correction_force_segmentation_threshold_chars();
    const int seg_max = get_correction_segment_max_chars();
    const int seg_overlap = get_correction_segment_overlap_chars();
    const bool segmented = static_cast<int>(raw_trimmed.size()) >= (std::max)(1, seg_threshold);

    if (info_out) {
        info_out->segmented = segmented;
    }

    if (!segmented) {
        std::string debug_stdout;
        if (!run_single_correction(llama_exe, llama_model, raw_trimmed, corrected_text, debug_stdout, error_out)) {
            return false;
        }
        if (info_out) {
            info_out->raw_stdout = debug_stdout;
            info_out->clean_output = corrected_text;
            info_out->segment_count = 1;
        }

        const bool is_short = non_space_count(raw_trimmed) < 8;
        if (is_short && !is_trivial_cleanup(raw_trimmed, corrected_text)) {
            corrected_text = raw_trimmed;
            if (info_out) {
                info_out->clean_output = corrected_text;
            }
        }
        return true;
    }

    const auto segments = build_segments(raw_trimmed, seg_max, seg_overlap);
    if (segments.empty()) {
        error_out = "Failed to build correction segments.";
        return false;
    }

    std::vector<std::string> corrected_segments;
    corrected_segments.reserve(segments.size());
    std::vector<int> failed;
    std::string raw_stdout_debug;

    for (std::size_t i = 0; i < segments.size(); ++i) {
        std::string seg_corrected;
        std::string seg_stdout;
        std::string seg_error;
        if (run_single_correction(llama_exe, llama_model, segments[i], seg_corrected, seg_stdout, seg_error)) {
            corrected_segments.push_back(seg_corrected);
        } else {
            corrected_segments.push_back(segments[i]);
            failed.push_back(static_cast<int>(i));
        }
        if (!seg_stdout.empty()) {
            if (!raw_stdout_debug.empty()) {
                raw_stdout_debug += " | ";
            }
            raw_stdout_debug += "seg" + std::to_string(i) + ":" + seg_stdout;
        }
    }

    corrected_text = merge_segments(corrected_segments, seg_overlap);
    if (corrected_text.empty()) {
        corrected_text = raw_trimmed;
    }

    if (info_out) {
        info_out->raw_stdout = compact_debug_excerpt(raw_stdout_debug, 500);
        info_out->clean_output = corrected_text;
        info_out->segment_count = static_cast<int>(segments.size());
        info_out->failed_segment_indices = failed;
    }

    if (!failed.empty()) {
        std::ostringstream err;
        err << "Segment fallback used for indices:";
        for (int idx : failed) {
            err << ' ' << idx;
        }
        error_out = err.str();
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
    std::cout << "[LLM_TEST_SEGMENTED] " << (info.segmented ? "true" : "false") << "\n";
    std::cout << "[LLM_TEST_SEGMENT_COUNT] " << info.segment_count << "\n";
    std::cout << "[LLM_TEST_MAX_OUTPUT_TOKENS] " << info.max_output_tokens << "\n";
    if (!info.raw_stdout.empty() && info.raw_stdout != info.clean_output) {
        std::cout << "[LLM_TEST_RAW_STDOUT] " << info.raw_stdout << "\n";
    }
    if (!info.clean_output.empty()) {
        std::cout << "[LLM_TEST_CLEAN_OUTPUT] " << info.clean_output << "\n";
    }

    if (!ok) {
        std::cout << "[LLM_TEST_ERROR] " << error << "\n";
        return 1;
    }

    std::cout << "[LLM_TEST_OUTPUT] " << corrected << "\n";
    std::cout << "[LLM_TEST_ERROR] " << error << "\n";
    return 0;
}
