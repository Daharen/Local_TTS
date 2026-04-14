#include "llm_correction.h"

#include "pipeline_debug.h"
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
    std::string startup_args_excerpt;
    std::string startup_probe_used;
    std::string startup_probe_response_excerpt;
    int startup_http_status = 0;
    PROCESS_INFORMATION process{};
    bool process_running = false;
};

struct ResidentStartupInfo {
    bool attempted = false;
    bool started = false;
    bool health_check_ok = false;
    int http_status = 0;
    std::string startup_error;
    std::string server_exe;
    std::string endpoint_used;
    std::string args_excerpt;
    std::string probe_response_excerpt;
};

struct ResidentRequestContext {
    int total_budget_ms = 0;
    int remaining_budget_ms = 0;
    int attempt_timeout_ms = 0;
    int request_count = 0;
    int last_status = 0;
    std::string phase;
    std::string last_endpoint;
    std::string last_error;
    std::string reset_reason;
};

struct HttpResult {
    bool transport_ok = false;
    int status_code = 0;
    std::string body_excerpt;
    std::string error_text;
};

ResidentBackendState& resident_state() {
    static ResidentBackendState state;
    return state;
}

void reset_resident_backend_state(const std::string& reason, bool log_marker) {
    auto& state = resident_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (log_marker) {
        std::cerr << "[LLM_RESIDENT_RESET] reason=" << compact_debug_excerpt(reason, 200) << "\n";
    }
    if (state.process.hProcess) {
        TerminateProcess(state.process.hProcess, 1);
        WaitForSingleObject(state.process.hProcess, 1500);
        CloseHandle(state.process.hProcess);
        state.process.hProcess = nullptr;
    }
    if (state.process.hThread) {
        CloseHandle(state.process.hThread);
        state.process.hThread = nullptr;
    }
    state.process_running = false;
    state.ready = false;
    state.startup_attempted = false;
    state.startup_error.clear();
    state.startup_http_status = 0;
    state.startup_probe_used.clear();
    state.startup_probe_response_excerpt.clear();
    state.startup_args_excerpt.clear();
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

HttpResult http_request_json(const std::string& host,
                             int port,
                             const std::wstring& method,
                             const std::wstring& path,
                             const std::string* body,
                             int timeout_ms) {
    HttpResult result;
    std::wstring host_w = utf8_to_wide(host);
    HINTERNET session = WinHttpOpen(L"LocalTTS/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error_text = "WinHttpOpen failed";
        return result;
    }

    HINTERNET connection = WinHttpConnect(session, host_w.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        result.error_text = "WinHttpConnect failed";
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        result.error_text = "WinHttpOpenRequest failed";
        return result;
    }
    WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    const wchar_t* headers = body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    const DWORD headers_len = body ? static_cast<DWORD>(wcslen(headers)) : 0;
    const DWORD body_size = body ? static_cast<DWORD>(body->size()) : 0;
    LPVOID body_data = body ? reinterpret_cast<LPVOID>(const_cast<char*>(body->data())) : WINHTTP_NO_REQUEST_DATA;
    BOOL ok = WinHttpSendRequest(request, headers, headers_len, body_data, body_size, body_size, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }

    if (ok) {
        result.transport_ok = true;
        DWORD status = 0;
        DWORD status_len = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status,
                            &status_len,
                            WINHTTP_NO_HEADER_INDEX);
        result.status_code = static_cast<int>(status);

        std::string response;
        DWORD size = 0;
        while (response.size() < 4096) {
            size = 0;
            if (!WinHttpQueryDataAvailable(request, &size) || size == 0) {
                break;
            }
            const DWORD take = (std::min<DWORD>)(size, 1024);
            std::string chunk(static_cast<std::size_t>(take), '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), take, &downloaded) || downloaded == 0) {
                break;
            }
            chunk.resize(downloaded);
            response += chunk;
        }
        if (response.size() > 4096) {
            response.resize(4096);
        }
        result.body_excerpt = response;
    } else {
        result.error_text = "HTTP request failed";
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

bool ping_resident_server(const std::string& host,
                          int port,
                          int timeout_ms,
                          std::string& probe_used_out,
                          int& status_out,
                          std::string& probe_excerpt_out) {
    static const std::vector<std::wstring> kPaths = {L"/health", L"/v1/models", L"/"};
    probe_used_out.clear();
    status_out = 0;
    probe_excerpt_out.clear();
    for (const auto& path : kPaths) {
        const HttpResult result = http_request_json(host, port, L"GET", path, nullptr, timeout_ms);
        if (!result.transport_ok) {
            continue;
        }
        probe_used_out = std::string(path.begin(), path.end());
        status_out = result.status_code;
        probe_excerpt_out = result.body_excerpt;
        return true;
    }
    return false;
}

std::string join_args_excerpt(const std::vector<std::wstring>& args) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty()) {
            out += " ";
        }
        out += compact_debug_excerpt(std::string(arg.begin(), arg.end()), 80);
        if (out.size() > 480) {
            break;
        }
    }
    return compact_debug_excerpt(out, 500);
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

bool start_resident_backend_if_needed(const std::filesystem::path& llama_model,
                                      int startup_timeout_ms,
                                      int budget_remaining_ms,
                                      ResidentStartupInfo& startup,
                                      ResidentRequestContext* ctx) {
    startup = {};
    startup.attempted = true;
    if (!is_correction_resident_enabled()) {
        startup.startup_error = "resident disabled in config";
        return false;
    }

    const std::string mode = to_lower_copy(trim_copy(get_correction_backend_mode()));
    if (!mode.empty() && mode != "resident" && mode != "auto") {
        startup.startup_error = "backend mode is not resident";
        return false;
    }

    auto& state = resident_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.ready && state.process_running) {
        std::cerr << "[LLM_RESIDENT_PROBE_BEGIN] endpoint=" << state.startup_probe_used << " timeout_ms=0 remaining_budget_ms="
                  << budget_remaining_ms << "\n";
        std::cerr << "[LLM_RESIDENT_PROBE_END] ok=true endpoint=" << state.startup_probe_used << " status=" << state.startup_http_status
                  << " remaining_budget_ms=" << budget_remaining_ms << "\n";
        std::cerr << "[LLM_RESIDENT_START_END] ok=true endpoint=" << state.startup_probe_used << " timeout_ms=0 remaining_budget_ms="
                  << budget_remaining_ms << "\n";
        startup.started = true;
        startup.health_check_ok = true;
        startup.server_exe = state.server_exe.string();
        startup.endpoint_used = state.startup_probe_used;
        startup.http_status = state.startup_http_status;
        startup.args_excerpt = state.startup_args_excerpt;
        startup.probe_response_excerpt = state.startup_probe_response_excerpt;
        if (ctx) {
            ctx->phase = "startup_ready";
        }
        return true;
    }
    state.startup_attempted = true;
    state.startup_error.clear();
    state.ready = false;
    state.startup_http_status = 0;
    state.startup_probe_used.clear();
    state.startup_probe_response_excerpt.clear();
    state.startup_args_excerpt.clear();
    state.server_exe = find_llama_server_executable_path(get_llama_cpp_root());
    startup.server_exe = state.server_exe.string();
    if (state.server_exe.empty()) {
        state.startup_error = "llama server executable not found under configured llama_cpp_root";
        state.startup_attempted = false;
        startup.startup_error = state.startup_error;
        if (ctx) {
            ctx->phase = "startup_failed";
            ctx->last_error = state.startup_error;
        }
        std::cerr << "[LLM_RESIDENT_START_END] ok=false endpoint=none timeout_ms=0 remaining_budget_ms=" << budget_remaining_ms
                  << " err=" << compact_debug_excerpt(state.startup_error, 180) << "\n";
        return false;
    }

    std::string help_text;
    std::string help_err;
    if (!run_process_capture_output(state.server_exe, {L"--help"}, help_text, help_err)) {
        help_text.clear();
    }
    std::cerr << "[LLM_RESIDENT_START_CONFIG] server_exe=" << compact_debug_excerpt(state.server_exe.string(), 240)
              << " model=" << compact_debug_excerpt(llama_model.string(), 240)
              << " host=" << get_correction_resident_host() << " port=" << get_correction_resident_port()
              << " startup_timeout_ms=" << startup_timeout_ms << " budget_remaining_ms=" << budget_remaining_ms
              << " help_chars=" << help_text.size() << " help_err_chars=" << help_err.size() << "\n";

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
    state.startup_args_excerpt = join_args_excerpt(args);
    startup.args_excerpt = state.startup_args_excerpt;
    std::cerr << "[LLM_RESIDENT_START_ARGS] " << compact_debug_excerpt(state.startup_args_excerpt, 500) << "\n";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = build_command_line(args);
    std::vector<wchar_t> cmdline(command.begin(), command.end());
    cmdline.push_back(L'\0');

    const BOOL started = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!started) {
        state.startup_error = "failed to start resident llama server process";
        state.startup_attempted = false;
        startup.startup_error = state.startup_error;
        std::cerr << "[LLM_RESIDENT_START] failed: " << state.startup_error << "\n";
        return false;
    }

    state.process = pi;
    state.process_running = true;
    state.llama_model = llama_model;

    startup.started = true;
    std::cerr << "[LLM_RESIDENT_PROBE_BEGIN] endpoint=health timeout_ms=" << startup_timeout_ms
              << " remaining_budget_ms=" << budget_remaining_ms << "\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(startup_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD exit_code = STILL_ACTIVE;
        if (GetExitCodeProcess(state.process.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
            state.process_running = false;
            state.startup_error = "resident server exited during startup (exit=" + std::to_string(static_cast<int>(exit_code)) + ")";
            startup.startup_error = state.startup_error;
            if (ctx) {
                ctx->phase = "startup_failed";
                ctx->last_error = state.startup_error;
            }
            break;
        }
        std::string probe_used;
        std::string probe_excerpt;
        int probe_status = 0;
        if (ping_resident_server(get_correction_resident_host(), get_correction_resident_port(), 350, probe_used, probe_status, probe_excerpt)) {
            state.ready = true;
            state.startup_probe_used = probe_used;
            state.startup_http_status = probe_status;
            state.startup_probe_response_excerpt = probe_excerpt;
            startup.health_check_ok = true;
            startup.endpoint_used = probe_used;
            startup.http_status = probe_status;
            startup.probe_response_excerpt = probe_excerpt;
            std::cerr << "[LLM_RESIDENT_PROBE_END] ok=true endpoint=" << probe_used << " status=" << probe_status
                      << " remaining_budget_ms=" << budget_remaining_ms << "\n";
            std::cerr << "[LLM_RESIDENT_START_END] ok=true endpoint=" << probe_used << " timeout_ms=" << startup_timeout_ms
                      << " remaining_budget_ms=" << budget_remaining_ms << "\n";
            if (ctx) {
                ctx->phase = "startup_ready";
                ctx->last_endpoint = probe_used;
                ctx->last_status = probe_status;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    if (!state.ready) {
        state.startup_error = state.startup_error.empty() ? "resident server startup timed out" : state.startup_error;
        state.startup_attempted = false;
        startup.startup_error = state.startup_error;
        std::cerr << "[LLM_RESIDENT_PROBE_END] ok=false endpoint=health status=0 remaining_budget_ms=" << budget_remaining_ms
                  << " err=" << compact_debug_excerpt(state.startup_error, 180) << "\n";
        std::cerr << "[LLM_RESIDENT_START_END] ok=false endpoint=none timeout_ms=" << startup_timeout_ms
                  << " remaining_budget_ms=" << budget_remaining_ms << " err=" << compact_debug_excerpt(state.startup_error, 180)
                  << "\n";
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
        if (ctx) {
            ctx->phase = "startup_failed";
            ctx->last_error = state.startup_error;
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
                                 std::string& sanitizer_reason,
                                 std::string& endpoint_used_out,
                                 int& http_status_out,
                                 bool& request_sent_out,
                                 std::string& resident_error_out,
                                 ResidentRequestContext& ctx) {
    endpoint_used_out.clear();
    http_status_out = 0;
    request_sent_out = false;
    resident_error_out.clear();
    const std::string sys_prompt = build_system_prompt();
    const std::string user_prompt = build_user_prompt(raw_trimmed);
    const std::string chat_body =
        "{\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(sys_prompt) + "\"},{\"role\":\"user\",\"content\":\"" +
        json_escape(user_prompt) + "\"}],\"temperature\":" + std::to_string(get_correction_temperature()) +
        ",\"top_k\":" + std::to_string(get_correction_top_k()) + ",\"top_p\":" + std::to_string(get_correction_top_p()) +
        ",\"min_p\":" + std::to_string(get_correction_min_p()) +
        ",\"n_predict\":" + std::to_string(get_correction_max_output_tokens()) + ",\"stream\":false}";
    const std::string completion_prompt = sys_prompt + "\n\n" + user_prompt + "\n\nOnly output rewritten text.";
    const std::string completion_body = "{\"prompt\":\"" + json_escape(completion_prompt) + "\",\"temperature\":" +
                                        std::to_string(get_correction_temperature()) + ",\"top_k\":" +
                                        std::to_string(get_correction_top_k()) + ",\"top_p\":" +
                                        std::to_string(get_correction_top_p()) + ",\"min_p\":" +
                                        std::to_string(get_correction_min_p()) + ",\"n_predict\":" +
                                        std::to_string(get_correction_max_output_tokens()) + ",\"stream\":false}";
    std::cerr << "[LLM_RESIDENT_INPUT] raw_chars=" << raw_trimmed.size() << " sys_prompt_chars=" << sys_prompt.size()
              << " user_prompt_chars=" << user_prompt.size() << " chat_body_chars=" << chat_body.size()
              << " completion_body_chars=" << completion_body.size() << "\n";

    struct Attempt {
        std::wstring path;
        const std::string* body;
    };
    const std::vector<Attempt> attempts = {{L"/v1/chat/completions", &chat_body}, {L"/v1/completions", &completion_body}, {L"/completion", &completion_body}};
    for (const auto& attempt : attempts) {
        if (ctx.remaining_budget_ms <= 0) {
            resident_error_out = "resident budget exhausted before request attempt";
            ctx.last_error = resident_error_out;
            break;
        }
        const int timeout = (std::min)(ctx.remaining_budget_ms, (std::max)(1, ctx.attempt_timeout_ms));
        request_sent_out = true;
        ctx.request_count += 1;
        endpoint_used_out = std::string(attempt.path.begin(), attempt.path.end());
        ctx.last_endpoint = endpoint_used_out;
        std::cerr << "[LLM_RESIDENT_REQUEST_BEGIN] endpoint=" << endpoint_used_out << " timeout_ms=" << timeout
                  << " remaining_budget_ms=" << ctx.remaining_budget_ms << " request_body_chars=" << attempt.body->size() << "\n";
        const auto attempt_begin = std::chrono::steady_clock::now();
        const HttpResult result = http_request_json(
            get_correction_resident_host(), get_correction_resident_port(), L"POST", attempt.path, attempt.body, timeout);
        const int elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attempt_begin).count());
        ctx.remaining_budget_ms = (std::max)(0, ctx.remaining_budget_ms - elapsed_ms);
        ctx.last_status = result.status_code;
        if (!result.transport_ok) {
            resident_error_out = result.error_text;
            ctx.last_error = resident_error_out;
            std::cerr << "[LLM_RESIDENT_REQUEST_END] endpoint=" << endpoint_used_out << " transport_ok=false status=0 extracted=false"
                      << " remaining_budget_ms=" << ctx.remaining_budget_ms
                      << " err=" << compact_debug_excerpt(resident_error_out, 180) << "\n";
            continue;
        }
        http_status_out = result.status_code;
        raw_stdout_excerpt = compact_debug_excerpt(result.body_excerpt, 700);
        raw_stderr_excerpt.clear();
        if (result.status_code < 200 || result.status_code >= 300) {
            resident_error_out = "resident HTTP status " + std::to_string(result.status_code);
            ctx.last_error = resident_error_out;
            std::cerr << "[LLM_RESIDENT_REQUEST_END] endpoint=" << endpoint_used_out << " transport_ok=true status="
                      << result.status_code << " extracted=false remaining_budget_ms=" << ctx.remaining_budget_ms
                      << " err=" << compact_debug_excerpt(resident_error_out, 180) << "\n";
            continue;
        }
        corrected_text = trim_copy(extract_content_from_server_response(result.body_excerpt));
        if (corrected_text.empty()) {
            corrected_text = trim_copy(extract_json_string_value(result.body_excerpt, "text"));
        }
        if (!corrected_text.empty()) {
            sanitizer_reason = "ok";
            ctx.last_error.clear();
            std::cerr << "[LLM_RESIDENT_REQUEST_END] endpoint=" << endpoint_used_out << " transport_ok=true status="
                      << result.status_code << " extracted=true remaining_budget_ms=" << ctx.remaining_budget_ms << "\n";
            return true;
        }
        resident_error_out = "resident response had no usable content";
        ctx.last_error = resident_error_out;
        std::cerr << "[LLM_RESIDENT_REQUEST_END] endpoint=" << endpoint_used_out << " transport_ok=true status=" << result.status_code
                  << " extracted=false remaining_budget_ms=" << ctx.remaining_budget_ms
                  << " err=" << compact_debug_excerpt(resident_error_out, 180) << "\n";
    }
    sanitizer_reason = "resident response had no usable content";
    error_out = resident_error_out.empty() ? sanitizer_reason : resident_error_out;
    return false;
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
    ResidentStartupInfo startup_info;
    std::string sanitizer_reason;
    std::string resident_endpoint_used;
    int resident_http_status = 0;
    bool resident_request_sent = false;
    std::string resident_error;
    ResidentRequestContext resident_ctx;
    resident_ctx.total_budget_ms = (std::max)(1, get_correction_resident_total_budget_ms());
    resident_ctx.remaining_budget_ms = resident_ctx.total_budget_ms;
    resident_ctx.attempt_timeout_ms = (std::max)(1, get_correction_resident_per_attempt_timeout_ms());
    resident_ctx.phase = "idle";
    backend_used = "none";
    if (info_out) {
        info_out->resident_attempted = false;
        info_out->resident_started = false;
        info_out->resident_health_check_ok = false;
        info_out->resident_request_sent = false;
        info_out->resident_http_status = 0;
        info_out->fallback_used = false;
        info_out->resident_phase = "idle";
        info_out->resident_total_budget_ms = resident_ctx.total_budget_ms;
        info_out->resident_remaining_budget_ms = resident_ctx.remaining_budget_ms;
        info_out->resident_attempt_timeout_ms = resident_ctx.attempt_timeout_ms;
        info_out->resident_request_count = 0;
        info_out->resident_last_status = 0;
        info_out->resident_error.clear();
        info_out->resident_last_error.clear();
        info_out->resident_reset_reason.clear();
        info_out->resident_startup_error.clear();
        info_out->resident_server_exe.clear();
        info_out->resident_last_endpoint.clear();
        info_out->resident_endpoint_used.clear();
        info_out->resident_probe_used.clear();
        info_out->resident_args_excerpt.clear();
        info_out->resident_probe_response_excerpt.clear();
        info_out->resident_gpu_layers = get_correction_resident_gpu_layers();
        info_out->resident_ctx_size = get_correction_resident_ctx_size();
        info_out->resident_threads = get_correction_resident_threads();
        info_out->oneshot_stderr_cuda_hint = false;
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
    if (prefer_resident) {
        const auto resident_begin = std::chrono::steady_clock::now();
        resident_ctx.phase = "startup";
        const int startup_timeout = (std::min)(resident_ctx.remaining_budget_ms, (std::max)(1, get_correction_resident_startup_timeout_ms()));
        std::cerr << "[LLM_RESIDENT_START_BEGIN] endpoint=process timeout_ms=" << startup_timeout
                  << " remaining_budget_ms=" << resident_ctx.remaining_budget_ms << "\n";
        if (resident_ctx.remaining_budget_ms <= 0) {
            startup_info.startup_error = "resident budget exhausted before startup";
            resident_ctx.last_error = startup_info.startup_error;
            std::cerr << "[LLM_RESIDENT_START_END] ok=false endpoint=none timeout_ms=0 remaining_budget_ms=0 err="
                      << compact_debug_excerpt(startup_info.startup_error, 180) << "\n";
        } else {
            const bool started = start_resident_backend_if_needed(
                llama_model, startup_timeout, resident_ctx.remaining_budget_ms, startup_info, &resident_ctx);
            const int startup_elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - resident_begin).count());
            resident_ctx.remaining_budget_ms = (std::max)(0, resident_ctx.total_budget_ms - startup_elapsed_ms);
            if (!started && resident_ctx.last_error.empty()) {
                resident_ctx.last_error = startup_info.startup_error;
            }
        }
    }
    if (prefer_resident && startup_info.health_check_ok) {
        if (info_out) {
            info_out->resident_started = startup_info.started;
            info_out->resident_health_check_ok = startup_info.health_check_ok;
            info_out->resident_server_exe = compact_debug_excerpt(startup_info.server_exe, 300);
            info_out->resident_startup_error = compact_debug_excerpt(startup_info.startup_error, 400);
            info_out->resident_probe_used = compact_debug_excerpt(startup_info.endpoint_used, 200);
            info_out->resident_args_excerpt = compact_debug_excerpt(startup_info.args_excerpt, 500);
            info_out->resident_probe_response_excerpt = compact_debug_excerpt(startup_info.probe_response_excerpt, 500);
            info_out->resident_http_status = startup_info.http_status;
        }
        if (request_resident_correction(
                raw_trimmed,
                corrected_text,
                error_out,
                stdout_text,
                stderr_text,
                sanitizer_reason,
                resident_endpoint_used,
                resident_http_status,
                resident_request_sent,
                resident_error,
                resident_ctx)) {
            backend_used = "resident";
            resident_ctx.phase = "request_ok";
            std::cerr << "[LLM_BACKEND] resident\n";
        } else {
            backend_used = "resident_failed_then_oneshot";
            resident_ctx.phase = "request_failed";
            resident_ctx.reset_reason = resident_error.empty() ? "resident request failed" : resident_error;
            std::cerr << "[LLM_RESIDENT_FALLBACK] endpoint=" << compact_debug_excerpt(resident_ctx.last_endpoint, 120)
                      << " status=" << resident_ctx.last_status << " remaining_budget_ms=" << resident_ctx.remaining_budget_ms
                      << " err=" << compact_debug_excerpt(resident_error.empty() ? error_out : resident_error, 200) << "\n";
            reset_resident_backend_state(resident_ctx.reset_reason, true);
            std::cerr << "[LLM_RESIDENT_FALLBACK] " << error_out << "\n";
        }
    } else if (prefer_resident && !startup_info.startup_error.empty()) {
        backend_used = "resident_failed_then_oneshot";
        resident_error = startup_info.startup_error;
        resident_ctx.phase = "startup_failed";
        resident_ctx.last_error = resident_error;
        std::cerr << "[LLM_RESIDENT_FALLBACK] " << startup_info.startup_error << "\n";
    }
    if (info_out) {
        info_out->resident_request_sent = resident_request_sent;
        info_out->resident_endpoint_used = compact_debug_excerpt(resident_endpoint_used, 200);
        info_out->resident_http_status = resident_http_status != 0 ? resident_http_status : info_out->resident_http_status;
        info_out->resident_error = compact_debug_excerpt(resident_error, 400);
        info_out->resident_phase = compact_debug_excerpt(resident_ctx.phase, 120);
        info_out->resident_total_budget_ms = resident_ctx.total_budget_ms;
        info_out->resident_remaining_budget_ms = resident_ctx.remaining_budget_ms;
        info_out->resident_attempt_timeout_ms = resident_ctx.attempt_timeout_ms;
        info_out->resident_request_count = resident_ctx.request_count;
        info_out->resident_last_endpoint = compact_debug_excerpt(resident_ctx.last_endpoint, 200);
        info_out->resident_last_status = resident_ctx.last_status;
        info_out->resident_last_error = compact_debug_excerpt(resident_ctx.last_error, 400);
        info_out->resident_reset_reason = compact_debug_excerpt(resident_ctx.reset_reason, 300);
    }

    if (corrected_text.empty()) {
        if (info_out && (info_out->resident_started || info_out->resident_attempted)) {
            info_out->fallback_used = true;
        }
        const std::string system_prompt = build_system_prompt();
        const std::string user_prompt = build_user_prompt(raw_trimmed);
        const LlamaFrontend frontend = detect_llama_frontend(llama_exe);
        if (!run_llama_process(
                llama_exe, llama_model, frontend, system_prompt, user_prompt, stdout_text, stderr_text, error_out, true) &&
            !run_llama_process(
                llama_exe, llama_model, frontend, system_prompt, user_prompt, stdout_text, stderr_text, error_out, false)) {
            if (backend_used == "none") {
                backend_used = (info_out && info_out->resident_attempted) ? "oneshot_fallback" : "oneshot";
            }
            if (info_out) {
                info_out->raw_stdout_excerpt = compact_debug_excerpt(stdout_text, 700);
                info_out->raw_stderr_excerpt = compact_debug_excerpt(stderr_text, 700);
                info_out->raw_error_text = compact_debug_excerpt(error_out, 400);
            }
            return false;
        }
        SanitizationResult sanitized = sanitize_llama_stdout(stdout_text, raw_trimmed);
        corrected_text = trim_copy(sanitized.output);
        sanitizer_reason = sanitized.reason;
        const std::string stderr_l = to_lower_copy(stderr_text);
        const bool saw_cuda = stderr_l.find("cuda") != std::string::npos;
        if (info_out) {
            info_out->oneshot_stderr_cuda_hint = saw_cuda;
        }
        if (backend_used == "resident_failed_then_oneshot") {
            backend_used = "resident_failed_then_oneshot";
        } else {
            backend_used = (info_out && info_out->resident_attempted) ? "oneshot_fallback" : "oneshot";
        }
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
    pipeline_debug::log("llm_correction", "correction requested");
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
        pipeline_debug::log("llm_correction", error_out, true);
        return false;
    }

#ifdef _WIN32
    std::filesystem::path llama_exe;
    std::filesystem::path llama_model;
    if (!resolve_llama_inputs(llama_exe, llama_model, error_out)) {
        pipeline_debug::log("llm_correction", error_out, true);
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
        const bool ok = run_single_correction(llama_exe, llama_model, raw_trimmed, corrected_text, error_out, info_out, backend_used);
        if (info_out && !backend_used.empty()) {
            info_out->backend_used = backend_used;
        }
        if (!ok) {
            pipeline_debug::log("llm_correction", error_out.empty() ? "single correction failed" : error_out, true);
            return false;
        }
        if (info_out) {
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
        pipeline_debug::log("llm_correction", error_out, true);
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
    CorrectionRunInfo last_seg_info;

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
            if (backend_used == "none" && !seg_backend.empty()) {
                backend_used = seg_backend;
            } else if (!seg_backend.empty() && seg_backend != backend_used) {
                backend_used = "mixed";
            }
        }
        any_resident_attempted = any_resident_attempted || seg_info.resident_attempted;
        any_resident_started = any_resident_started || seg_info.resident_started;
        any_fallback_used = any_fallback_used || seg_info.fallback_used;
        last_seg_info = seg_info;
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
        info_out->resident_phase = last_seg_info.resident_phase;
        info_out->resident_total_budget_ms = last_seg_info.resident_total_budget_ms;
        info_out->resident_remaining_budget_ms = last_seg_info.resident_remaining_budget_ms;
        info_out->resident_attempt_timeout_ms = last_seg_info.resident_attempt_timeout_ms;
        info_out->resident_request_count = last_seg_info.resident_request_count;
        info_out->resident_last_endpoint = last_seg_info.resident_last_endpoint;
        info_out->resident_last_status = last_seg_info.resident_last_status;
        info_out->resident_last_error = last_seg_info.resident_last_error;
        info_out->resident_reset_reason = last_seg_info.resident_reset_reason;
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
        pipeline_debug::log("llm_correction", error_out, true);
    }

    pipeline_debug::log("llm_correction", "correction completed using backend=" + backend_used +
                                             " segmented=" + std::string(segmented ? "true" : "false"));

    return true;
#else
    error_out = "LLM correction is only supported on Windows.";
    pipeline_debug::log("llm_correction", error_out, true);
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
    std::cout << "[LLM_TEST_RESIDENT_ATTEMPTED] " << (info.resident_attempted ? "true" : "false") << "\n";
    std::cout << "[LLM_TEST_RESIDENT_STARTED] " << (info.resident_started ? "true" : "false") << "\n";
    std::cout << "[LLM_TEST_RESIDENT_HEALTH_OK] " << (info.resident_health_check_ok ? "true" : "false") << "\n";
    std::cout << "[LLM_TEST_RESIDENT_ENDPOINT] " << info.resident_endpoint_used << "\n";
    std::cout << "[LLM_TEST_RESIDENT_HTTP_STATUS] " << info.resident_http_status << "\n";
    std::cout << "[LLM_TEST_RESIDENT_ERROR] " << info.resident_error << "\n";
    std::cout << "[LLM_TEST_RESIDENT_PHASE] " << info.resident_phase << "\n";
    std::cout << "[LLM_TEST_RESIDENT_BUDGET_MS] " << info.resident_total_budget_ms << "\n";
    std::cout << "[LLM_TEST_RESIDENT_REMAINING_BUDGET_MS] " << info.resident_remaining_budget_ms << "\n";
    std::cout << "[LLM_TEST_RESIDENT_ATTEMPT_TIMEOUT_MS] " << info.resident_attempt_timeout_ms << "\n";
    std::cout << "[LLM_TEST_RESIDENT_REQUEST_COUNT] " << info.resident_request_count << "\n";
    std::cout << "[LLM_TEST_RESIDENT_LAST_ENDPOINT] " << info.resident_last_endpoint << "\n";
    std::cout << "[LLM_TEST_RESIDENT_LAST_STATUS] " << info.resident_last_status << "\n";
    std::cout << "[LLM_TEST_RESIDENT_LAST_ERROR] " << info.resident_last_error << "\n";
    std::cout << "[LLM_TEST_RESIDENT_RESET_REASON] " << info.resident_reset_reason << "\n";
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
    reset_resident_backend_state("shutdown", false);
#endif
}
