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

std::string find_json_value_token(const std::string& json, const std::string& key_name) {
    const std::string key = "\"" + key_name + "\"";
    const auto key_pos = json.find(key);
    if (key_pos == std::string::npos) {
        return {};
    }

    auto value_pos = json.find(':', key_pos + key.size());
    if (value_pos == std::string::npos) {
        return {};
    }

    ++value_pos;
    while (value_pos < json.size() && (json[value_pos] == ' ' || json[value_pos] == '\t' || json[value_pos] == '\r' || json[value_pos] == '\n')) {
        ++value_pos;
    }

    if (value_pos >= json.size()) {
        return {};
    }

    if (json[value_pos] == '"') {
        return parse_json_string_at(json, value_pos);
    }

    const auto value_end = json.find_first_of(",}\r\n\t ", value_pos);
    if (value_end == std::string::npos) {
        return trim_copy(json.substr(value_pos));
    }
    return trim_copy(json.substr(value_pos, value_end - value_pos));
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
    return trim_copy(find_json_value_token(config_content, key_name));
}

bool parse_bool_value(const std::string& value, bool fallback) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return fallback;
}

int parse_int_value(const std::string& value, int fallback) {
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

double parse_double_value(const std::string& value, double fallback) {
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
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

std::filesystem::path get_llama_cpp_root() {
    const std::filesystem::path fallback = R"(F:\Qwen3.5-27B\llama.cpp)";

    const auto from_runtime = read_runtime_value("llama_cpp_root");
    if (!from_runtime.empty()) {
        return std::filesystem::path(from_runtime);
    }

    if (const char* env = std::getenv("LOCAL_TTS_LLAMA_CPP_ROOT")) {
        const auto from_env = trim_copy(env);
        if (!from_env.empty()) {
            return std::filesystem::path(from_env);
        }
    }

    return fallback;
}

std::filesystem::path get_llama_model_path() {
    const std::filesystem::path fallback = R"(F:\Qwen3.5-27B\small-model-3b\Qwen2.5-3B-Instruct-IQ4_XS.gguf)";

    const auto from_runtime = read_runtime_value("llama_model_path");
    if (!from_runtime.empty()) {
        return std::filesystem::path(from_runtime);
    }

    if (const char* env = std::getenv("LOCAL_TTS_LLAMA_MODEL_PATH")) {
        const auto from_env = trim_copy(env);
        if (!from_env.empty()) {
            return std::filesystem::path(from_env);
        }
    }

    return fallback;
}

bool is_correction_enabled() {
    const auto runtime_value = read_runtime_value("correction_enabled");
    if (!runtime_value.empty()) {
        return parse_bool_value(runtime_value, true);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_ENABLED")) {
        return parse_bool_value(trim_copy(env), true);
    }

    return true;
}

double get_correction_temperature() {
    const auto runtime_value = read_runtime_value("correction_temperature");
    if (!runtime_value.empty()) {
        return parse_double_value(runtime_value, 0.0);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_TEMPERATURE")) {
        return parse_double_value(trim_copy(env), 0.0);
    }

    return 0.0;
}

int get_correction_top_k() {
    const auto runtime_value = read_runtime_value("correction_top_k");
    if (!runtime_value.empty()) {
        return parse_int_value(runtime_value, 1);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_TOP_K")) {
        return parse_int_value(trim_copy(env), 1);
    }

    return 1;
}

double get_correction_top_p() {
    const auto runtime_value = read_runtime_value("correction_top_p");
    if (!runtime_value.empty()) {
        return parse_double_value(runtime_value, 0.0);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_TOP_P")) {
        return parse_double_value(trim_copy(env), 0.0);
    }

    return 0.0;
}

double get_correction_min_p() {
    const auto runtime_value = read_runtime_value("correction_min_p");
    if (!runtime_value.empty()) {
        return parse_double_value(runtime_value, 0.0);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_MIN_P")) {
        return parse_double_value(trim_copy(env), 0.0);
    }

    return 0.0;
}


int get_correction_max_output_tokens() {
    const auto runtime_value = read_runtime_value("correction_max_output_tokens");
    if (!runtime_value.empty()) {
        return parse_int_value(runtime_value, 512);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_MAX_OUTPUT_TOKENS")) {
        return parse_int_value(trim_copy(env), 512);
    }

    return 512;
}

int get_correction_segment_max_chars() {
    const auto runtime_value = read_runtime_value("correction_segment_max_chars");
    if (!runtime_value.empty()) {
        return parse_int_value(runtime_value, 1600);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_SEGMENT_MAX_CHARS")) {
        return parse_int_value(trim_copy(env), 1600);
    }

    return 1600;
}

int get_correction_segment_overlap_chars() {
    const auto runtime_value = read_runtime_value("correction_segment_overlap_chars");
    if (!runtime_value.empty()) {
        return parse_int_value(runtime_value, 200);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_SEGMENT_OVERLAP_CHARS")) {
        return parse_int_value(trim_copy(env), 200);
    }

    return 200;
}

int get_correction_force_segmentation_threshold_chars() {
    const auto runtime_value = read_runtime_value("correction_force_segmentation_threshold_chars");
    if (!runtime_value.empty()) {
        return parse_int_value(runtime_value, 1800);
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_FORCE_SEGMENTATION_THRESHOLD_CHARS")) {
        return parse_int_value(trim_copy(env), 1800);
    }

    return 1800;
}

std::string get_correction_mode() {
    const auto runtime_value = trim_copy(read_runtime_value("correction_mode"));
    if (!runtime_value.empty()) {
        return runtime_value;
    }

    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_MODE")) {
        const auto from_env = trim_copy(env);
        if (!from_env.empty()) {
            return from_env;
        }
    }

    return "formatted";
}

std::string describe_paths_json() {
    const auto repo = get_repo_root().string();
    const auto data = get_large_data_root().string();
    const auto whisper_cpp = get_whisper_cpp_root().string();
    const auto whisper_model = get_whisper_model_path().string();
    const auto llama_cpp = get_llama_cpp_root().string();
    const auto llama_model = get_llama_model_path().string();

    std::ostringstream out;
    out << "{\n"
        << "  \"repo_root\": \"" << escape_json(repo) << "\",\n"
        << "  \"large_data_root\": \"" << escape_json(data) << "\",\n"
        << "  \"whisper_cpp_root\": \"" << escape_json(whisper_cpp) << "\",\n"
        << "  \"whisper_model_path\": \"" << escape_json(whisper_model) << "\",\n"
        << "  \"llama_cpp_root\": \"" << escape_json(llama_cpp) << "\",\n"
        << "  \"llama_model_path\": \"" << escape_json(llama_model) << "\",\n"
        << "  \"correction_enabled\": " << (is_correction_enabled() ? "true" : "false") << ",\n"
        << "  \"correction_temperature\": " << get_correction_temperature() << ",\n"
        << "  \"correction_top_k\": " << get_correction_top_k() << ",\n"
        << "  \"correction_top_p\": " << get_correction_top_p() << ",\n"
        << "  \"correction_min_p\": " << get_correction_min_p() << ",\n"
        << "  \"correction_max_output_tokens\": " << get_correction_max_output_tokens() << ",\n"
        << "  \"correction_segment_max_chars\": " << get_correction_segment_max_chars() << ",\n"
        << "  \"correction_segment_overlap_chars\": " << get_correction_segment_overlap_chars() << ",\n"
        << "  \"correction_force_segmentation_threshold_chars\": " << get_correction_force_segmentation_threshold_chars() << ",\n"
        << "  \"correction_mode\": \"" << escape_json(get_correction_mode()) << "\"\n"
        << "}";
    return out.str();
}
