#include "llm_correction.h"

#include "paths.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
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

struct SanitizationResult {
    std::string output;
    std::string reason;
};

SanitizationResult sanitize_llama_stdout(const std::string& raw_stdout, const std::string& raw_trimmed) {
    SanitizationResult result{};
    const std::string normalized = normalize_newlines(raw_stdout);
    if (trim_copy(normalized).empty()) {
        result.reason = "empty stdout";
        return result;
    }
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

    bool saw_meta_only = false;
    bool saw_raw_match = false;
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        const std::string block = trim_copy(*it);
        if (!has_meaningful_text(block)) {
            continue;
        }
        const std::string lower = to_lower_copy(block);
        if (is_prompt_echo_line(block, lower) || is_shell_help_line(block, lower) || is_footer_line(block, lower)) {
            saw_meta_only = true;
            continue;
        }
        if (block_matches_raw_transcript(block, raw_trimmed)) {
            saw_raw_match = true;
            result.output = block;
            continue;
        }
        result.output = block;
        result.reason = "ok";
        return result;
    }

    // Conservative fallback: keep the last meaningful non-noise line.
    std::vector<std::string> meaningful;
    for (const std::string& raw_line : split_lines(normalized)) {
        const std::string line = trim_copy(raw_line);
        if (line.empty()) {
            continue;
        }
        const std::string lower = to_lower_copy(line);
        const std::string line_norm = collapse_space_copy(line);
        if (is_banner_line(lower) || is_shell_help_line(line, lower) || is_footer_line(line, lower) ||
            is_prompt_echo_line(line, lower) || is_raw_transcript_echo_line(line_norm, raw_norm, raw_single_line_norm)) {
            continue;
        }
        if (looks_like_meta_output(line)) {
            continue;
        }
        if (!has_meaningful_text(line)) {
            continue;
        }
        meaningful.push_back(line);
    }

    if (!meaningful.empty()) {
        result.output = trim_copy(meaningful.back());
        result.reason = "fallback_last_meaningful_line";
        if (block_matches_raw_transcript(result.output, raw_trimmed)) {
            result.reason = "output matched raw transcript exactly";
        }
        return result;
    }

    if (saw_raw_match) {
        result.reason = "output matched raw transcript exactly";
    } else if (candidate_lines.empty()) {
        result.reason = "only shell noise remained";
    } else if (saw_meta_only) {
        result.reason = "only meta output remained";
    } else if (blocks.empty()) {
        result.reason = "no structured payload block found";
    } else {
        result.reason = "extracted payload was too weak";
    }
    return result;
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

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (const unsigned char ch : input) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(static_cast<char>(ch)); break;
        }
    }
    return out;
}

std::string extract_json_string_value(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const auto key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
        return {};
    }
    const auto colon = json.find(':', key_pos + needle.size());
    if (colon == std::string::npos) {
        return {};
    }
    auto value_pos = json.find('"', colon + 1);
    if (value_pos == std::string::npos) {
        return {};
    }
    std::string out;
    for (std::size_t i = value_pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\') {
            if (i + 1 >= json.size()) {
                return {};
            }
            const char next = json[++i];
            switch (next) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(next); break;
            }
            continue;
        }
        if (c == '"') {
            return out;
        }
        out.push_back(c);
    }
    return {};
}

std::string extract_content_from_server_response(const std::string& body) {
    std::string content = extract_json_string_value(body, "content");
    if (!content.empty()) {
        return content;
    }

    const auto msg_pos = body.find("\"message\"");
    if (msg_pos != std::string::npos) {
        content = extract_json_string_value(body.substr(msg_pos), "content");
        if (!content.empty()) {
            return content;
        }
    }
    return {};
}

bool run_process_capture_output(const std::filesystem::path& exe,
                                const std::vector<std::wstring>& args,
                                std::string& output,
                                std::string& error_out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0) || !SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
        error_out = "Failed to create process capture pipe.";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_handle;
    si.hStdError = write_handle;

    PROCESS_INFORMATION pi{};
    std::vector<std::wstring> full_args;
    full_args.push_back(exe.wstring());
    full_args.insert(full_args.end(), args.begin(), args.end());
    std::wstring command = build_command_line(full_args);
    std::vector<wchar_t> cmdline(command.begin(), command.end());
    cmdline.push_back(L'\0');

    const BOOL started = CreateProcessW(nullptr,
                                        cmdline.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &si,
                                        &pi);
    CloseHandle(write_handle);
    if (!started) {
        CloseHandle(read_handle);
        error_out = "Failed to run process for capability detection.";
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    read_handle_all(read_handle, output);
    CloseHandle(read_handle);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct ResidentBackendState {
    std::mutex mutex;
    bool startup_attempted = false;
    bool ready = false;
    std::string startup_error;
    std::filesystem::path server_exe;
    std::filesystem::path llama_model;
    PROCESS_INFORMATION process{};
    bool process_running = false;
};

ResidentBackendState& resident_state() {
    static ResidentBackendState state;
    return state;
}

std::filesystem::path find_llama_server_executable_path(const std::filesystem::path& llama_root) {
    const std::vector<std::filesystem::path> candidates = {
        llama_root / "build" / "bin" / "Release" / "llama-server.exe",
        llama_root / "build" / "bin" / "llama-server.exe",
        llama_root / "build" / "Release" / "llama-server.exe",
        llama_root / "build" / "llama-server.exe",
        llama_root / "build" / "bin" / "Release" / "server.exe",
        llama_root / "build" / "bin" / "server.exe",
        llama_root / "build" / "Release" / "server.exe",
        llama_root / "build" / "server.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool process_supports_flag(const std::string& help_text, const std::string& flag) {
    return help_text.find(flag) != std::string::npos;
}

bool ping_resident_server(const std::string& host, int port, int timeout_ms, std::string* response_out = nullptr) {
    std::wstring host_w = utf8_to_wide(host);
    HINTERNET session = WinHttpOpen(L"LocalTTS/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, host_w.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/health", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok && response_out) {
        response_out->clear();
        DWORD size = 0;
        do {
            size = 0;
            if (!WinHttpQueryDataAvailable(request, &size) || size == 0) {
                break;
            }
            std::string chunk(size, '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), size, &downloaded) || downloaded == 0) {
                break;
            }
            chunk.resize(downloaded);
            response_out->append(chunk);
        } while (size > 0);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
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

bool start_resident_backend_if_needed(const std::filesystem::path& llama_model, std::string& reason_out) {
    reason_out.clear();
    if (!is_correction_resident_enabled()) {
        reason_out = "resident disabled in config";
        return false;
    }

    const std::string mode = to_lower_copy(trim_copy(get_correction_backend_mode()));
    if (!mode.empty() && mode != "resident" && mode != "auto") {
        reason_out = "backend mode is not resident";
        return false;
    }

    auto& state = resident_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.ready && state.process_running) {
        return true;
    }
    if (state.startup_attempted && !state.ready) {
        reason_out = state.startup_error.empty() ? "previous resident startup failed" : state.startup_error;
        return false;
    }

    state.startup_attempted = true;
    state.startup_error.clear();
    state.ready = false;
    state.server_exe = find_llama_server_executable_path(get_llama_cpp_root());
    if (state.server_exe.empty()) {
        state.startup_error = "llama server executable not found under configured llama_cpp_root";
        reason_out = state.startup_error;
        std::cerr << "[LLM_RESIDENT_START] failed: " << state.startup_error << "\n";
        return false;
    }

    std::string help_text;
    std::string help_err;
    if (!run_process_capture_output(state.server_exe, {L"--help"}, help_text, help_err)) {
        help_text.clear();
    }

    std::vector<std::wstring> args = {state.server_exe.wstring(), L"-m", llama_model.wstring()};
    const std::wstring host_w = utf8_to_wide(get_correction_resident_host());
    const int port = get_correction_resident_port();

    if (process_supports_flag(help_text, "--host")) {
        args.push_back(L"--host");
        args.push_back(host_w);
    } else if (process_supports_flag(help_text, "-host")) {
        args.push_back(L"-host");
        args.push_back(host_w);
    }

    if (process_supports_flag(help_text, "--port")) {
        args.push_back(L"--port");
        args.push_back(number_to_wide(port));
    } else if (process_supports_flag(help_text, "-port")) {
        args.push_back(L"-port");
        args.push_back(number_to_wide(port));
    }

    if (process_supports_flag(help_text, "--ctx-size")) {
        args.push_back(L"--ctx-size");
        args.push_back(number_to_wide(get_correction_resident_ctx_size()));
    } else if (process_supports_flag(help_text, "-c")) {
        args.push_back(L"-c");
        args.push_back(number_to_wide(get_correction_resident_ctx_size()));
    }

    if (process_supports_flag(help_text, "--threads")) {
        args.push_back(L"--threads");
        args.push_back(number_to_wide(get_correction_resident_threads()));
    } else if (process_supports_flag(help_text, "-t")) {
        args.push_back(L"-t");
        args.push_back(number_to_wide(get_correction_resident_threads()));
    }

    if (process_supports_flag(help_text, "--gpu-layers")) {
        args.push_back(L"--gpu-layers");
        args.push_back(number_to_wide(get_correction_resident_gpu_layers()));
    } else if (process_supports_flag(help_text, "-ngl")) {
        args.push_back(L"-ngl");
        args.push_back(number_to_wide(get_correction_resident_gpu_layers()));
    }

    if (process_supports_flag(help_text, "--jinja")) {
        args.push_back(L"--jinja");
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = build_command_line(args);
    std::vector<wchar_t> cmdline(command.begin(), command.end());
    cmdline.push_back(L'\0');

    const BOOL started = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!started) {
        state.startup_error = "failed to start resident llama server process";
        reason_out = state.startup_error;
        std::cerr << "[LLM_RESIDENT_START] failed: " << state.startup_error << "\n";
        return false;
    }

    state.process = pi;
    state.process_running = true;
    state.llama_model = llama_model;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(get_correction_resident_startup_timeout_ms());
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD exit_code = STILL_ACTIVE;
        if (GetExitCodeProcess(state.process.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
            state.process_running = false;
            state.startup_error = "resident server exited during startup";
            reason_out = state.startup_error;
            break;
        }
        if (ping_resident_server(get_correction_resident_host(), get_correction_resident_port(), 300)) {
            state.ready = true;
            std::cerr << "[LLM_RESIDENT_START] ok\n";
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    if (!state.ready) {
        state.startup_error = state.startup_error.empty() ? "resident server startup timed out" : state.startup_error;
        reason_out = state.startup_error;
        std::cerr << "[LLM_RESIDENT_START] failed: " << state.startup_error << "\n";
        if (state.process_running) {
            TerminateProcess(state.process.hProcess, 1);
            state.process_running = false;
        }
        if (state.process.hThread) {
            CloseHandle(state.process.hThread);
            state.process.hThread = nullptr;
        }
        if (state.process.hProcess) {
            CloseHandle(state.process.hProcess);
            state.process.hProcess = nullptr;
        }
        return false;
    }
    return true;
}

bool request_resident_correction(const std::string& raw_trimmed,
                                 std::string& corrected_text,
                                 std::string& error_out,
                                 std::string& raw_stdout_excerpt,
                                 std::string& raw_stderr_excerpt,
                                 std::string& sanitizer_reason) {
    const std::string sys_prompt = build_system_prompt();
    const std::string user_prompt = build_user_prompt(raw_trimmed);
    const std::string body =
        "{\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(sys_prompt) + "\"},{\"role\":\"user\",\"content\":\"" +
        json_escape(user_prompt) + "\"}],\"temperature\":" + std::to_string(get_correction_temperature()) +
        ",\"top_k\":" + std::to_string(get_correction_top_k()) + ",\"top_p\":" + std::to_string(get_correction_top_p()) +
        ",\"min_p\":" + std::to_string(get_correction_min_p()) +
        ",\"n_predict\":" + std::to_string(get_correction_max_output_tokens()) + "}";

    HINTERNET session = WinHttpOpen(L"LocalTTS/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error_out = "WinHttpOpen failed";
        return false;
    }

    const std::wstring host_w = utf8_to_wide(get_correction_resident_host());
    HINTERNET connection =
        WinHttpConnect(session, host_w.c_str(), static_cast<INTERNET_PORT>(get_correction_resident_port()), 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        error_out = "WinHttpConnect failed";
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"POST", L"/v1/chat/completions", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error_out = "WinHttpOpenRequest failed";
        return false;
    }

    const int timeout = get_correction_resident_request_timeout_ms();
    WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
    const std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                 reinterpret_cast<LPVOID>(const_cast<char*>(body.data())), static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
    ok = ok && WinHttpReceiveResponse(request, nullptr);

    std::string response;
    if (ok) {
        DWORD size = 0;
        do {
            size = 0;
            if (!WinHttpQueryDataAvailable(request, &size) || size == 0) {
                break;
            }
            std::string chunk(size, '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), size, &downloaded) || downloaded == 0) {
                break;
            }
            chunk.resize(downloaded);
            response += chunk;
        } while (size > 0);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok) {
        error_out = "resident HTTP request failed";
        return false;
    }

    raw_stdout_excerpt = compact_debug_excerpt(response, 700);
    raw_stderr_excerpt.clear();
    corrected_text = trim_copy(extract_content_from_server_response(response));
    if (corrected_text.empty()) {
        sanitizer_reason = "resident response had no usable content";
        error_out = "resident response had no usable content";
        return false;
    }
    sanitizer_reason = "ok";
    return true;
}

bool run_single_correction(const std::filesystem::path& llama_exe,
                           const std::filesystem::path& llama_model,
                           const std::string& raw_trimmed,
                           std::string& corrected_text,
                           std::string& error_out,
                           CorrectionRunInfo* info_out,
                           std::string& backend_used) {
    std::string stdout_text;
    std::string stderr_text;
    std::string resident_reason;
    std::string sanitizer_reason;
    if (info_out) {
        info_out->resident_attempted = false;
        info_out->resident_started = false;
        info_out->fallback_used = false;
        info_out->raw_stdout_excerpt.clear();
        info_out->raw_stderr_excerpt.clear();
        info_out->raw_error_text.clear();
        info_out->sanitizer_reason.clear();
        info_out->sanitized_output.clear();
    }
    const std::string mode = to_lower_copy(trim_copy(get_correction_backend_mode()));
    const bool prefer_resident = mode.empty() || mode == "resident" || mode == "auto";
    if (info_out) {
        info_out->resident_attempted = prefer_resident;
    }
    if (prefer_resident && start_resident_backend_if_needed(llama_model, resident_reason)) {
        if (info_out) {
            info_out->resident_started = true;
        }
        if (request_resident_correction(
                raw_trimmed, corrected_text, error_out, stdout_text, stderr_text, sanitizer_reason)) {
            backend_used = "resident";
            std::cerr << "[LLM_BACKEND] resident\n";
        } else {
            std::cerr << "[LLM_RESIDENT_FALLBACK] " << error_out << "\n";
        }
    } else if (prefer_resident && !resident_reason.empty()) {
        std::cerr << "[LLM_RESIDENT_FALLBACK] " << resident_reason << "\n";
    }

    if (corrected_text.empty()) {
        if (info_out && info_out->resident_started) {
            info_out->fallback_used = true;
        }
        const std::string system_prompt = build_system_prompt();
        const std::string user_prompt = build_user_prompt(raw_trimmed);
        const LlamaFrontend frontend = detect_llama_frontend(llama_exe);
        if (!run_llama_process(
                llama_exe, llama_model, frontend, system_prompt, user_prompt, stdout_text, stderr_text, error_out, true) &&
            !run_llama_process(
                llama_exe, llama_model, frontend, system_prompt, user_prompt, stdout_text, stderr_text, error_out, false)) {
            return false;
        }
        SanitizationResult sanitized = sanitize_llama_stdout(stdout_text, raw_trimmed);
        corrected_text = trim_copy(sanitized.output);
        sanitizer_reason = sanitized.reason;
        backend_used = (info_out && info_out->resident_started) ? "oneshot_fallback" : "oneshot";
        std::cerr << "[LLM_BACKEND] " << backend_used << "\n";
    }

    if (info_out) {
        info_out->raw_stdout_excerpt = compact_debug_excerpt(stdout_text, 700);
        info_out->raw_stderr_excerpt = compact_debug_excerpt(stderr_text, 700);
        info_out->sanitized_output = compact_debug_excerpt(corrected_text, 700);
    }

    if (corrected_text.empty()) {
        if (sanitizer_reason.empty()) {
            sanitizer_reason = "extracted payload was too weak";
        }
        if (info_out) {
            info_out->sanitizer_reason = sanitizer_reason;
            info_out->raw_error_text = compact_debug_excerpt(error_out, 400);
        }
        error_out = "Correction output rejected: " + sanitizer_reason;
        return false;
    }

    if (looks_like_meta_output(corrected_text)) {
        sanitizer_reason = "only meta output remained";
        if (info_out) {
            info_out->sanitizer_reason = sanitizer_reason;
            info_out->raw_error_text = compact_debug_excerpt(error_out, 400);
        }
        error_out = "Correction output rejected: " + sanitizer_reason;
        return false;
    }

    if (block_matches_raw_transcript(corrected_text, raw_trimmed)) {
        sanitizer_reason = "output matched raw transcript exactly";
        if (info_out) {
            info_out->sanitizer_reason = sanitizer_reason;
            info_out->raw_error_text = compact_debug_excerpt(error_out, 400);
        }
        error_out = "Correction output matched raw transcript.";
        return false;
    }

    if (info_out) {
        info_out->sanitizer_reason = sanitizer_reason.empty() ? "ok" : sanitizer_reason;
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
        info_out->backend_used = "none";
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
        std::string backend_used;
        if (!run_single_correction(llama_exe, llama_model, raw_trimmed, corrected_text, error_out, info_out, backend_used)) {
            return false;
        }
        if (info_out) {
            info_out->backend_used = backend_used;
            info_out->raw_stdout = info_out->raw_stdout_excerpt;
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
    std::string raw_stderr_debug;
    std::string sanitizer_reason_debug;
    std::string backend_used = "none";
    bool any_resident_attempted = false;
    bool any_resident_started = false;
    bool any_fallback_used = false;

    for (std::size_t i = 0; i < segments.size(); ++i) {
        std::string seg_corrected;
        std::string seg_error;
        std::string seg_backend;
        CorrectionRunInfo seg_info;
        if (run_single_correction(llama_exe, llama_model, segments[i], seg_corrected, seg_error, &seg_info, seg_backend)) {
            corrected_segments.push_back(seg_corrected);
            if (backend_used == "none") {
                backend_used = seg_backend;
            } else if (!seg_backend.empty() && seg_backend != backend_used) {
                backend_used = "mixed";
            }
        } else {
            corrected_segments.push_back(segments[i]);
            failed.push_back(static_cast<int>(i));
        }
        any_resident_attempted = any_resident_attempted || seg_info.resident_attempted;
        any_resident_started = any_resident_started || seg_info.resident_started;
        any_fallback_used = any_fallback_used || seg_info.fallback_used;
        if (!seg_info.raw_stdout_excerpt.empty()) {
            if (!raw_stdout_debug.empty()) {
                raw_stdout_debug += " | ";
            }
            raw_stdout_debug += "seg" + std::to_string(i) + ":" + seg_info.raw_stdout_excerpt;
        }
        if (!seg_info.raw_stderr_excerpt.empty()) {
            if (!raw_stderr_debug.empty()) {
                raw_stderr_debug += " | ";
            }
            raw_stderr_debug += "seg" + std::to_string(i) + ":" + seg_info.raw_stderr_excerpt;
        }
        if (!seg_info.sanitizer_reason.empty()) {
            if (!sanitizer_reason_debug.empty()) {
                sanitizer_reason_debug += " | ";
            }
            sanitizer_reason_debug += "seg" + std::to_string(i) + ":" + seg_info.sanitizer_reason;
        }
    }

    corrected_text = merge_segments(corrected_segments, seg_overlap);
    if (corrected_text.empty()) {
        corrected_text = raw_trimmed;
    }

    if (info_out) {
        info_out->backend_used = backend_used;
        info_out->raw_stdout_excerpt = compact_debug_excerpt(raw_stdout_debug, 700);
        info_out->raw_stderr_excerpt = compact_debug_excerpt(raw_stderr_debug, 700);
        info_out->raw_stdout = info_out->raw_stdout_excerpt;
        info_out->sanitizer_reason = compact_debug_excerpt(sanitizer_reason_debug, 500);
        info_out->sanitized_output = compact_debug_excerpt(corrected_text, 700);
        info_out->resident_attempted = any_resident_attempted;
        info_out->resident_started = any_resident_started;
        info_out->fallback_used = any_fallback_used;
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
    std::cout << "[LLM_TEST_BACKEND] " << info.backend_used << "\n";
    std::cout << "[LLM_TEST_SEGMENTED] " << (info.segmented ? "true" : "false") << "\n";
    std::cout << "[LLM_TEST_SEGMENT_COUNT] " << info.segment_count << "\n";
    std::cout << "[LLM_TEST_MAX_OUTPUT_TOKENS] " << info.max_output_tokens << "\n";
    std::cout << "[LLM_TEST_RAW_STDOUT] " << info.raw_stdout_excerpt << "\n";
    std::cout << "[LLM_TEST_RAW_STDERR] " << info.raw_stderr_excerpt << "\n";
    std::cout << "[LLM_TEST_SANITIZER_REASON] " << info.sanitizer_reason << "\n";
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

void shutdown_llm_correction_backend() {
#ifdef _WIN32
    auto& state = resident_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (!state.process_running) {
        return;
    }
    if (state.process.hProcess) {
        TerminateProcess(state.process.hProcess, 0);
        WaitForSingleObject(state.process.hProcess, 2000);
        CloseHandle(state.process.hProcess);
        state.process.hProcess = nullptr;
    }
    if (state.process.hThread) {
        CloseHandle(state.process.hThread);
        state.process.hThread = nullptr;
    }
    state.process_running = false;
    state.ready = false;
#endif
}
