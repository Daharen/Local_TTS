#include "paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string trim_copy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string parse_json_string_at(const std::string& text, std::size_t start_quote) {
    std::string out;
    for (std::size_t i = start_quote + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\') {
            if (i + 1 >= text.size()) {
                return {};
            }
            const char n = text[++i];
            switch (n) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: return {};
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

std::string find_json_string_value(const std::string& json, const std::string& key_name) {
    const std::string key = "\"" + key_name + "\"";
    const auto key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return {};
    }

    auto colon_pos = json.find(':', key_pos + key.size());
    if (colon_pos == std::string::npos) {
        return {};
    }

    ++colon_pos;
    while (colon_pos < json.size() && (json[colon_pos] == ' ' || json[colon_pos] == '\t' || json[colon_pos] == '\r' || json[colon_pos] == '\n')) {
        ++colon_pos;
    }

    if (colon_pos >= json.size() || json[colon_pos] != '"') {
        return {};
    }

    return parse_json_string_at(json, colon_pos);
}

std::string escape_json(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (const char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string read_runtime_value(const std::string& key_name) {
    const auto config_content = read_file(get_repo_root() / "runtime.local.json");
    if (config_content.empty()) {
        return {};
    }
    return trim_copy(find_json_string_value(config_content, key_name));
}

}  // namespace

std::filesystem::path get_repo_root() {
    return std::filesystem::current_path();
}

std::filesystem::path get_large_data_root() {
    const std::filesystem::path fallback = R"(F:\Local_TTS_Large_Data)";

    const auto from_runtime = read_runtime_value("large_data_root");
    if (!from_runtime.empty()) {
        return std::filesystem::path(from_runtime);
    }

    if (const char* env = std::getenv("LOCAL_TTS_LARGE_DATA_ROOT")) {
        const auto from_env = trim_copy(env);
        if (!from_env.empty()) {
            return std::filesystem::path(from_env);
        }
    }

    return fallback;
}

std::filesystem::path get_whisper_cpp_root() {
    const auto from_runtime = read_runtime_value("whisper_cpp_root");
    if (!from_runtime.empty()) {
        return std::filesystem::path(from_runtime);
    }

    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_CPP_ROOT")) {
        const auto from_env = trim_copy(env);
        if (!from_env.empty()) {
            return std::filesystem::path(from_env);
        }
    }

    return get_large_data_root() / "external" / "whisper.cpp";
}

std::filesystem::path get_whisper_model_path() {
    const auto from_runtime = read_runtime_value("whisper_model_path");
    if (!from_runtime.empty()) {
        return std::filesystem::path(from_runtime);
    }

    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_MODEL_PATH")) {
        const auto from_env = trim_copy(env);
        if (!from_env.empty()) {
            return std::filesystem::path(from_env);
        }
    }

    return get_large_data_root() / "models" / "whisper.cpp" / "ggml-base.en.bin";
}

std::string describe_paths_json() {
    const auto repo = get_repo_root().string();
    const auto data = get_large_data_root().string();
    const auto whisper_cpp = get_whisper_cpp_root().string();
    const auto whisper_model = get_whisper_model_path().string();

    std::ostringstream out;
    out << "{\n"
        << "  \"repo_root\": \"" << escape_json(repo) << "\",\n"
        << "  \"large_data_root\": \"" << escape_json(data) << "\",\n"
        << "  \"whisper_cpp_root\": \"" << escape_json(whisper_cpp) << "\",\n"
        << "  \"whisper_model_path\": \"" << escape_json(whisper_model) << "\"\n"
        << "}";
    return out.str();
}
