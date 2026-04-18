#include "paths.h"

#include "app_config.h"

#include <sstream>

namespace {

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

}  // namespace

std::filesystem::path get_repo_root() {
    return std::filesystem::current_path();
}

std::filesystem::path get_large_data_root() {
    return get_app_config().large_data_root;
}

std::filesystem::path get_whisper_cpp_root() {
    return get_app_config().whisper_cpp_root;
}

std::filesystem::path get_whisper_model_path() {
    return get_app_config().whisper_model_path;
}

std::filesystem::path get_whisper_cli_path_override() {
    return get_app_config().whisper_cli_path;
}

bool is_whisper_gpu_requested() {
    return get_app_config().whisper_use_gpu;
}

int get_whisper_gpu_device() {
    return get_app_config().whisper_gpu_device;
}

bool is_whisper_flash_attn_enabled() {
    return get_app_config().whisper_flash_attn;
}

int get_whisper_threads() {
    return get_app_config().whisper_threads;
}

std::filesystem::path get_llama_cpp_root() {
    return get_app_config().llama_cpp_root;
}

std::filesystem::path get_llama_model_path() {
    return get_app_config().llama_model_path;
}

bool is_pipeline_debug_enabled() {
    return get_app_config().pipeline_debug_enabled;
}

bool is_correction_enabled() {
    return get_app_config().correction_enabled;
}

double get_correction_temperature() {
    return get_app_config().correction_temperature;
}

int get_correction_top_k() {
    return get_app_config().correction_top_k;
}

double get_correction_top_p() {
    return get_app_config().correction_top_p;
}

double get_correction_min_p() {
    return get_app_config().correction_min_p;
}

int get_correction_max_output_tokens() {
    return get_app_config().correction_max_output_tokens;
}

int get_correction_segment_max_chars() {
    return get_app_config().correction_segment_max_chars;
}

int get_correction_segment_overlap_chars() {
    return get_app_config().correction_segment_overlap_chars;
}

int get_correction_force_segmentation_threshold_chars() {
    return get_app_config().correction_force_segmentation_threshold_chars;
}

std::string get_correction_mode() {
    return get_app_config().correction_mode;
}

std::string get_correction_backend_mode() {
    return get_app_config().correction_backend_mode;
}

bool is_correction_resident_enabled() {
    return get_app_config().correction_resident_enabled;
}

std::string get_correction_resident_host() {
    return get_app_config().correction_resident_host;
}

int get_correction_resident_port() {
    return get_app_config().correction_resident_port;
}

int get_correction_resident_ctx_size() {
    return get_app_config().correction_resident_ctx_size;
}

int get_correction_resident_gpu_layers() {
    return get_app_config().correction_resident_gpu_layers;
}

int get_correction_resident_threads() {
    return get_app_config().correction_resident_threads;
}

int get_correction_resident_startup_timeout_ms() {
    return get_app_config().correction_resident_startup_timeout_ms;
}

int get_correction_resident_request_timeout_ms() {
    return get_app_config().correction_resident_request_timeout_ms;
}

int get_correction_resident_total_budget_ms() {
    return get_app_config().correction_resident_total_budget_ms;
}

int get_correction_resident_per_attempt_timeout_ms() {
    return get_app_config().correction_resident_per_attempt_timeout_ms;
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
        << "  \"whisper_cli_path\": \"" << escape_json(get_whisper_cli_path_override().string()) << "\",\n"
        << "  \"whisper_use_gpu\": " << (is_whisper_gpu_requested() ? "true" : "false") << ",\n"
        << "  \"whisper_gpu_device\": " << get_whisper_gpu_device() << ",\n"
        << "  \"whisper_flash_attn\": " << (is_whisper_flash_attn_enabled() ? "true" : "false") << ",\n"
        << "  \"whisper_threads\": " << get_whisper_threads() << ",\n"
        << "  \"llama_cpp_root\": \"" << escape_json(llama_cpp) << "\",\n"
        << "  \"llama_model_path\": \"" << escape_json(llama_model) << "\",\n"
        << "  \"pipeline_debug_enabled\": " << (is_pipeline_debug_enabled() ? "true" : "false") << ",\n"
        << "  \"correction_enabled\": " << (is_correction_enabled() ? "true" : "false") << ",\n"
        << "  \"correction_temperature\": " << get_correction_temperature() << ",\n"
        << "  \"correction_top_k\": " << get_correction_top_k() << ",\n"
        << "  \"correction_top_p\": " << get_correction_top_p() << ",\n"
        << "  \"correction_min_p\": " << get_correction_min_p() << ",\n"
        << "  \"correction_max_output_tokens\": " << get_correction_max_output_tokens() << ",\n"
        << "  \"correction_segment_max_chars\": " << get_correction_segment_max_chars() << ",\n"
        << "  \"correction_segment_overlap_chars\": " << get_correction_segment_overlap_chars() << ",\n"
        << "  \"correction_force_segmentation_threshold_chars\": " << get_correction_force_segmentation_threshold_chars() << ",\n"
        << "  \"correction_mode\": \"" << escape_json(get_correction_mode()) << "\",\n"
        << "  \"correction_backend_mode\": \"" << escape_json(get_correction_backend_mode()) << "\",\n"
        << "  \"correction_resident_enabled\": " << (is_correction_resident_enabled() ? "true" : "false") << ",\n"
        << "  \"correction_resident_host\": \"" << escape_json(get_correction_resident_host()) << "\",\n"
        << "  \"correction_resident_port\": " << get_correction_resident_port() << ",\n"
        << "  \"correction_resident_ctx_size\": " << get_correction_resident_ctx_size() << ",\n"
        << "  \"correction_resident_gpu_layers\": " << get_correction_resident_gpu_layers() << ",\n"
        << "  \"correction_resident_threads\": " << get_correction_resident_threads() << ",\n"
        << "  \"correction_resident_startup_timeout_ms\": " << get_correction_resident_startup_timeout_ms() << ",\n"
        << "  \"correction_resident_request_timeout_ms\": " << get_correction_resident_request_timeout_ms() << ",\n"
        << "  \"correction_resident_total_budget_ms\": " << get_correction_resident_total_budget_ms() << ",\n"
        << "  \"correction_resident_per_attempt_timeout_ms\": " << get_correction_resident_per_attempt_timeout_ms() << "\n"
        << "}";
    return out.str();
}
