#include "app_config.h"

#include "paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

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

void apply_config_json(AppConfig& config, const std::string& json) {
    if (json.empty()) {
        return;
    }

    const auto large_data_root = find_json_value_token(json, "large_data_root");
    if (!large_data_root.empty()) {
        config.large_data_root = large_data_root;
    }

    const auto whisper_cpp_root = find_json_value_token(json, "whisper_cpp_root");
    if (!whisper_cpp_root.empty()) {
        config.whisper_cpp_root = whisper_cpp_root;
    }

    const auto whisper_model_path = find_json_value_token(json, "whisper_model_path");
    if (!whisper_model_path.empty()) {
        config.whisper_model_path = whisper_model_path;
    }

    const auto whisper_cli_path = find_json_value_token(json, "whisper_cli_path");
    if (!whisper_cli_path.empty()) {
        config.whisper_cli_path = whisper_cli_path;
    }

    const auto whisper_use_gpu = find_json_value_token(json, "whisper_use_gpu");
    if (!whisper_use_gpu.empty()) {
        config.whisper_use_gpu = parse_bool_value(whisper_use_gpu, config.whisper_use_gpu);
    }

    const auto whisper_gpu_device = find_json_value_token(json, "whisper_gpu_device");
    if (!whisper_gpu_device.empty()) {
        config.whisper_gpu_device = parse_int_value(whisper_gpu_device, config.whisper_gpu_device);
    }

    const auto whisper_flash_attn = find_json_value_token(json, "whisper_flash_attn");
    if (!whisper_flash_attn.empty()) {
        config.whisper_flash_attn = parse_bool_value(whisper_flash_attn, config.whisper_flash_attn);
    }

    const auto whisper_threads = find_json_value_token(json, "whisper_threads");
    if (!whisper_threads.empty()) {
        config.whisper_threads = parse_int_value(whisper_threads, config.whisper_threads);
    }

    const auto llama_cpp_root = find_json_value_token(json, "llama_cpp_root");
    if (!llama_cpp_root.empty()) {
        config.llama_cpp_root = llama_cpp_root;
    }

    const auto llama_model_path = find_json_value_token(json, "llama_model_path");
    if (!llama_model_path.empty()) {
        config.llama_model_path = llama_model_path;
    }

    const auto pipeline_debug_enabled = find_json_value_token(json, "pipeline_debug_enabled");
    if (!pipeline_debug_enabled.empty()) {
        config.pipeline_debug_enabled = parse_bool_value(pipeline_debug_enabled, config.pipeline_debug_enabled);
    }

    const auto correction_enabled = find_json_value_token(json, "correction_enabled");
    if (!correction_enabled.empty()) {
        config.correction_enabled = parse_bool_value(correction_enabled, config.correction_enabled);
    }

    const auto correction_temperature = find_json_value_token(json, "correction_temperature");
    if (!correction_temperature.empty()) {
        config.correction_temperature = parse_double_value(correction_temperature, config.correction_temperature);
    }

    const auto correction_top_k = find_json_value_token(json, "correction_top_k");
    if (!correction_top_k.empty()) {
        config.correction_top_k = parse_int_value(correction_top_k, config.correction_top_k);
    }

    const auto correction_top_p = find_json_value_token(json, "correction_top_p");
    if (!correction_top_p.empty()) {
        config.correction_top_p = parse_double_value(correction_top_p, config.correction_top_p);
    }

    const auto correction_min_p = find_json_value_token(json, "correction_min_p");
    if (!correction_min_p.empty()) {
        config.correction_min_p = parse_double_value(correction_min_p, config.correction_min_p);
    }

    const auto correction_mode = trim_copy(find_json_value_token(json, "correction_mode"));
    if (!correction_mode.empty()) {
        config.correction_mode = correction_mode;
    }

    const auto correction_backend_mode = trim_copy(find_json_value_token(json, "correction_backend_mode"));
    if (!correction_backend_mode.empty()) {
        config.correction_backend_mode = correction_backend_mode;
    }

    const auto correction_resident_enabled = find_json_value_token(json, "correction_resident_enabled");
    if (!correction_resident_enabled.empty()) {
        config.correction_resident_enabled = parse_bool_value(correction_resident_enabled, config.correction_resident_enabled);
    }

    const auto correction_resident_host = trim_copy(find_json_value_token(json, "correction_resident_host"));
    if (!correction_resident_host.empty()) {
        config.correction_resident_host = correction_resident_host;
    }

    const auto correction_resident_port = find_json_value_token(json, "correction_resident_port");
    if (!correction_resident_port.empty()) {
        config.correction_resident_port = parse_int_value(correction_resident_port, config.correction_resident_port);
    }

    const auto correction_resident_ctx_size = find_json_value_token(json, "correction_resident_ctx_size");
    if (!correction_resident_ctx_size.empty()) {
        config.correction_resident_ctx_size = parse_int_value(correction_resident_ctx_size, config.correction_resident_ctx_size);
    }

    const auto correction_resident_gpu_layers = find_json_value_token(json, "correction_resident_gpu_layers");
    if (!correction_resident_gpu_layers.empty()) {
        config.correction_resident_gpu_layers = parse_int_value(correction_resident_gpu_layers, config.correction_resident_gpu_layers);
    }

    const auto correction_resident_threads = find_json_value_token(json, "correction_resident_threads");
    if (!correction_resident_threads.empty()) {
        config.correction_resident_threads = parse_int_value(correction_resident_threads, config.correction_resident_threads);
    }

    const auto correction_resident_startup_timeout_ms = find_json_value_token(json, "correction_resident_startup_timeout_ms");
    if (!correction_resident_startup_timeout_ms.empty()) {
        config.correction_resident_startup_timeout_ms =
            parse_int_value(correction_resident_startup_timeout_ms, config.correction_resident_startup_timeout_ms);
    }

    const auto correction_resident_request_timeout_ms = find_json_value_token(json, "correction_resident_request_timeout_ms");
    if (!correction_resident_request_timeout_ms.empty()) {
        config.correction_resident_request_timeout_ms =
            parse_int_value(correction_resident_request_timeout_ms, config.correction_resident_request_timeout_ms);
    }
    const auto correction_resident_total_budget_ms = find_json_value_token(json, "correction_resident_total_budget_ms");
    if (!correction_resident_total_budget_ms.empty()) {
        config.correction_resident_total_budget_ms =
            parse_int_value(correction_resident_total_budget_ms, config.correction_resident_total_budget_ms);
    }
    const auto correction_resident_per_attempt_timeout_ms =
        find_json_value_token(json, "correction_resident_per_attempt_timeout_ms");
    if (!correction_resident_per_attempt_timeout_ms.empty()) {
        config.correction_resident_per_attempt_timeout_ms =
            parse_int_value(correction_resident_per_attempt_timeout_ms, config.correction_resident_per_attempt_timeout_ms);
    }

    const auto correction_max_output_tokens = find_json_value_token(json, "correction_max_output_tokens");
    if (!correction_max_output_tokens.empty()) {
        config.correction_max_output_tokens = parse_int_value(correction_max_output_tokens, config.correction_max_output_tokens);
    }

    const auto correction_segment_max_chars = find_json_value_token(json, "correction_segment_max_chars");
    if (!correction_segment_max_chars.empty()) {
        config.correction_segment_max_chars = parse_int_value(correction_segment_max_chars, config.correction_segment_max_chars);
    }

    const auto correction_segment_overlap_chars = find_json_value_token(json, "correction_segment_overlap_chars");
    if (!correction_segment_overlap_chars.empty()) {
        config.correction_segment_overlap_chars = parse_int_value(correction_segment_overlap_chars, config.correction_segment_overlap_chars);
    }

    const auto correction_force_segmentation_threshold_chars = find_json_value_token(json, "correction_force_segmentation_threshold_chars");
    if (!correction_force_segmentation_threshold_chars.empty()) {
        config.correction_force_segmentation_threshold_chars = parse_int_value(correction_force_segmentation_threshold_chars, config.correction_force_segmentation_threshold_chars);
    }
}

void apply_environment_overrides(AppConfig& config) {
    if (const char* env = std::getenv("LOCAL_TTS_LARGE_DATA_ROOT")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.large_data_root = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_CPP_ROOT")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.whisper_cpp_root = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_MODEL_PATH")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.whisper_model_path = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_CLI_PATH")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.whisper_cli_path = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_USE_GPU")) {
        config.whisper_use_gpu = parse_bool_value(trim_copy(env), config.whisper_use_gpu);
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_GPU_DEVICE")) {
        config.whisper_gpu_device = parse_int_value(trim_copy(env), config.whisper_gpu_device);
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_FLASH_ATTN")) {
        config.whisper_flash_attn = parse_bool_value(trim_copy(env), config.whisper_flash_attn);
    }
    if (const char* env = std::getenv("LOCAL_TTS_WHISPER_THREADS")) {
        config.whisper_threads = parse_int_value(trim_copy(env), config.whisper_threads);
    }
    if (const char* env = std::getenv("LOCAL_TTS_LLAMA_CPP_ROOT")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.llama_cpp_root = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_LLAMA_MODEL_PATH")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.llama_model_path = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_PIPELINE_DEBUG_ENABLED")) {
        config.pipeline_debug_enabled = parse_bool_value(trim_copy(env), config.pipeline_debug_enabled);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_ENABLED")) {
        config.correction_enabled = parse_bool_value(trim_copy(env), config.correction_enabled);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_TEMPERATURE")) {
        config.correction_temperature = parse_double_value(trim_copy(env), config.correction_temperature);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_TOP_K")) {
        config.correction_top_k = parse_int_value(trim_copy(env), config.correction_top_k);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_TOP_P")) {
        config.correction_top_p = parse_double_value(trim_copy(env), config.correction_top_p);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_MIN_P")) {
        config.correction_min_p = parse_double_value(trim_copy(env), config.correction_min_p);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_MODE")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.correction_mode = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_BACKEND_MODE")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.correction_backend_mode = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_ENABLED")) {
        config.correction_resident_enabled = parse_bool_value(trim_copy(env), config.correction_resident_enabled);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_HOST")) {
        const auto value = trim_copy(env);
        if (!value.empty()) {
            config.correction_resident_host = value;
        }
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_PORT")) {
        config.correction_resident_port = parse_int_value(trim_copy(env), config.correction_resident_port);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_CTX_SIZE")) {
        config.correction_resident_ctx_size = parse_int_value(trim_copy(env), config.correction_resident_ctx_size);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_GPU_LAYERS")) {
        config.correction_resident_gpu_layers = parse_int_value(trim_copy(env), config.correction_resident_gpu_layers);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_THREADS")) {
        config.correction_resident_threads = parse_int_value(trim_copy(env), config.correction_resident_threads);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_STARTUP_TIMEOUT_MS")) {
        config.correction_resident_startup_timeout_ms =
            parse_int_value(trim_copy(env), config.correction_resident_startup_timeout_ms);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_REQUEST_TIMEOUT_MS")) {
        config.correction_resident_request_timeout_ms =
            parse_int_value(trim_copy(env), config.correction_resident_request_timeout_ms);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_TOTAL_BUDGET_MS")) {
        config.correction_resident_total_budget_ms = parse_int_value(trim_copy(env), config.correction_resident_total_budget_ms);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_RESIDENT_PER_ATTEMPT_TIMEOUT_MS")) {
        config.correction_resident_per_attempt_timeout_ms =
            parse_int_value(trim_copy(env), config.correction_resident_per_attempt_timeout_ms);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_MAX_OUTPUT_TOKENS")) {
        config.correction_max_output_tokens = parse_int_value(trim_copy(env), config.correction_max_output_tokens);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_SEGMENT_MAX_CHARS")) {
        config.correction_segment_max_chars = parse_int_value(trim_copy(env), config.correction_segment_max_chars);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_SEGMENT_OVERLAP_CHARS")) {
        config.correction_segment_overlap_chars = parse_int_value(trim_copy(env), config.correction_segment_overlap_chars);
    }
    if (const char* env = std::getenv("LOCAL_TTS_CORRECTION_FORCE_SEGMENTATION_THRESHOLD_CHARS")) {
        config.correction_force_segmentation_threshold_chars = parse_int_value(trim_copy(env), config.correction_force_segmentation_threshold_chars);
    }
}

AppConfig make_default_config() {
    const std::filesystem::path large_data_root = R"(F:\Local_TTS_Large_Data)";

    AppConfig config{};
    config.large_data_root = large_data_root;
    config.whisper_cpp_root = large_data_root / "external" / "whisper.cpp";
    config.whisper_model_path = large_data_root / "models" / "whisper.cpp" / "ggml-base.en.bin";
    config.whisper_cli_path.clear();
    config.whisper_use_gpu = true;
    config.whisper_gpu_device = 0;
    config.whisper_flash_attn = false;
    config.whisper_threads = 0;
    config.llama_cpp_root = R"(F:\Qwen3.5-27B\llama.cpp)";
    config.llama_model_path = R"(F:\Qwen3.5-27B\small-model-3b\Qwen2.5-3B-Instruct-IQ4_XS.gguf)";
    config.pipeline_debug_enabled = false;

    config.correction_enabled = true;
    config.correction_temperature = 0.0;
    config.correction_top_k = 1;
    config.correction_top_p = 0.0;
    config.correction_min_p = 0.0;
    config.correction_mode = "formatted";
    config.correction_backend_mode = "oneshot";
    config.correction_resident_enabled = true;
    config.correction_resident_host = "127.0.0.1";
    config.correction_resident_port = 18081;
    config.correction_resident_ctx_size = 4096;
    config.correction_resident_gpu_layers = -1;
    config.correction_resident_threads = 8;
    config.correction_resident_startup_timeout_ms = 20000;
    config.correction_resident_request_timeout_ms = 15000;
    config.correction_resident_total_budget_ms = 35000;
    config.correction_resident_per_attempt_timeout_ms = 2500;
    config.correction_max_output_tokens = 512;
    config.correction_segment_max_chars = 1600;
    config.correction_segment_overlap_chars = 200;
    config.correction_force_segmentation_threshold_chars = 1800;

    return config;
}

AppConfig load_app_config() {
    AppConfig config = make_default_config();
    const auto repo_root = get_repo_root();

    apply_config_json(config, read_file(repo_root / "runtime.repo.json"));
    apply_config_json(config, read_file(repo_root / "runtime.local.json"));
    apply_environment_overrides(config);

    return config;
}

}  // namespace

const AppConfig& get_app_config() {
    static const AppConfig config = load_app_config();
    return config;
}
