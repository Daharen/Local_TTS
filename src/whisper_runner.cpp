#include "whisper_runner.h"

#include "paths.h"
#include "pipeline_debug.h"

#include "whisper.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

template <typename T, typename = void>
struct has_gpu_device_field : std::false_type {};

template <typename T>
struct has_gpu_device_field<T, std::void_t<decltype(std::declval<T&>().gpu_device)>> : std::true_type {};

template <typename T, typename = void>
struct has_no_context_field : std::false_type {};

template <typename T>
struct has_no_context_field<T, std::void_t<decltype(std::declval<T&>().no_context)>> : std::true_type {};

struct WavData {
    std::vector<float> samples;
    int sample_rate = 0;
};

struct ResidentWhisperState {
    std::mutex mutex;
    std::filesystem::path model_path;
    bool initialized = false;
    bool init_attempted = false;
    bool use_gpu = false;
    int gpu_device = 0;
    bool flash_attn = false;
    int threads = 0;
    whisper_context* ctx = nullptr;

    ~ResidentWhisperState() {
        if (ctx != nullptr) {
            whisper_free(ctx);
            ctx = nullptr;
        }
    }
};

ResidentWhisperState& resident_state() {
    static ResidentWhisperState state;
    return state;
}

std::string trim_copy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
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

int resolve_threads() {
    const int configured = get_whisper_threads();
    return configured > 0 ? configured : 4;
}

bool resolve_transcribe_inputs(
    const std::filesystem::path& audio_path,
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

    return true;
}

bool read_exact(std::ifstream& in, void* dst, std::size_t n) {
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
    return in.good() || in.gcount() == static_cast<std::streamsize>(n);
}

bool load_wav_pcm16_mono_16khz(const std::filesystem::path& audio_path, WavData& out, std::string& error_out) {
    std::ifstream in(audio_path, std::ios::binary);
    if (!in) {
        error_out = "Failed to open wav file: " + audio_path.string();
        return false;
    }

    std::array<char, 4> riff{};
    std::uint32_t riff_size = 0;
    std::array<char, 4> wave{};
    if (!read_exact(in, riff.data(), riff.size()) || !read_exact(in, &riff_size, sizeof(riff_size)) ||
        !read_exact(in, wave.data(), wave.size())) {
        error_out = "Invalid WAV header: truncated RIFF/WAVE header.";
        return false;
    }

    if (std::memcmp(riff.data(), "RIFF", 4) != 0 || std::memcmp(wave.data(), "WAVE", 4) != 0) {
        error_out = "Invalid WAV header: expected RIFF/WAVE.";
        return false;
    }

    bool found_fmt = false;
    bool found_data = false;
    std::uint16_t audio_format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<std::int16_t> pcm;

    while (in) {
        std::array<char, 4> chunk_id{};
        std::uint32_t chunk_size = 0;
        if (!read_exact(in, chunk_id.data(), chunk_id.size())) {
            break;
        }
        if (!read_exact(in, &chunk_size, sizeof(chunk_size))) {
            error_out = "Invalid WAV header: truncated chunk size.";
            return false;
        }

        if (std::memcmp(chunk_id.data(), "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                error_out = "Unsupported WAV fmt chunk: too small.";
                return false;
            }

            std::uint16_t block_align = 0;
            std::uint32_t byte_rate = 0;
            if (!read_exact(in, &audio_format, sizeof(audio_format)) || !read_exact(in, &channels, sizeof(channels)) ||
                !read_exact(in, &sample_rate, sizeof(sample_rate)) || !read_exact(in, &byte_rate, sizeof(byte_rate)) ||
                !read_exact(in, &block_align, sizeof(block_align)) || !read_exact(in, &bits_per_sample, sizeof(bits_per_sample))) {
                error_out = "Invalid WAV fmt chunk: truncated fields.";
                return false;
            }

            const std::uint32_t remaining = chunk_size - 16;
            if (remaining > 0) {
                in.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
            }
            found_fmt = true;
        } else if (std::memcmp(chunk_id.data(), "data", 4) == 0) {
            if (chunk_size % sizeof(std::int16_t) != 0) {
                error_out = "Unsupported WAV data chunk: expected 16-bit PCM alignment.";
                return false;
            }
            pcm.resize(chunk_size / sizeof(std::int16_t));
            if (!read_exact(in, pcm.data(), chunk_size)) {
                error_out = "Invalid WAV data chunk: truncated payload.";
                return false;
            }
            found_data = true;
        } else {
            in.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        }

        if (chunk_size % 2 != 0) {
            in.seekg(1, std::ios::cur);
        }
    }

    if (!found_fmt) {
        error_out = "Invalid WAV file: missing fmt chunk.";
        return false;
    }
    if (!found_data) {
        error_out = "Invalid WAV file: missing data chunk.";
        return false;
    }

    if (audio_format != 1) {
        error_out = "Unsupported WAV format: only PCM is supported.";
        return false;
    }
    if (channels != 1) {
        error_out = "Unsupported WAV format: only mono channel is supported.";
        return false;
    }
    if (sample_rate != 16000) {
        error_out = "Unsupported WAV format: expected sample rate 16000 Hz.";
        return false;
    }
    if (bits_per_sample != 16) {
        error_out = "Unsupported WAV format: expected 16-bit samples.";
        return false;
    }

    out.sample_rate = static_cast<int>(sample_rate);
    out.samples.resize(pcm.size());
    constexpr float kScale = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        out.samples[i] = static_cast<float>(pcm[i]) * kScale;
    }

    return true;
}

std::string build_profile_excerpt(bool use_gpu, int gpu_device, bool flash_attn, int threads) {
    std::ostringstream out;
    out << "in_process use_gpu=" << (use_gpu ? "true" : "false")
        << " gpu_device=" << gpu_device
        << " flash_attn=" << (flash_attn ? "true" : "false")
        << " threads=" << threads
        << " sampling=greedy single_segment=true no_timestamps=true temp_inc=0";
    return out.str();
}

bool ensure_resident_context(
    ResidentWhisperState& state,
    const std::filesystem::path& model_path,
    bool use_gpu,
    int gpu_device,
    bool flash_attn,
    int threads,
    bool& init_reused,
    double& init_ms,
    std::string& error_out) {
    const bool config_changed = !state.initialized || state.model_path != model_path || state.use_gpu != use_gpu ||
                                state.gpu_device != gpu_device || state.flash_attn != flash_attn || state.threads != threads;

    if (!config_changed) {
        init_reused = true;
        init_ms = 0.0;
        return true;
    }

    init_reused = false;

    if (state.ctx != nullptr) {
        whisper_free(state.ctx);
        state.ctx = nullptr;
    }

    auto init_started = Clock::now();

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = use_gpu;
    cparams.flash_attn = flash_attn;
    if constexpr (has_gpu_device_field<whisper_context_params>::value) {
        cparams.gpu_device = gpu_device;
    }

    const std::string model_path_string = model_path.string();
    state.ctx = whisper_init_from_file_with_params(model_path_string.c_str(), cparams);
    init_ms = std::chrono::duration<double, std::milli>(Clock::now() - init_started).count();

    state.init_attempted = true;
    state.model_path = model_path;
    state.use_gpu = use_gpu;
    state.gpu_device = gpu_device;
    state.flash_attn = flash_attn;
    state.threads = threads;
    state.initialized = state.ctx != nullptr;

    if (state.ctx == nullptr) {
        error_out = "Failed to initialize whisper context for model: " + model_path.string();
        return false;
    }

    return true;
}

std::string summarize_timings(double init_ms, double wav_ms, double infer_ms, double extract_ms, double total_ms) {
    std::ostringstream out;
    out << "init_ms=" << init_ms
        << " wav_ms=" << wav_ms
        << " infer_ms=" << infer_ms
        << " extract_ms=" << extract_ms
        << " total_ms=" << total_ms;
    return out.str();
}

bool transcribe_pcm_locked_with_info(
    ResidentWhisperState& state,
    const std::filesystem::path& model_path,
    const float* pcm,
    int pcm_sample_count,
    double wav_ms,
    std::string& text_out,
    std::string& error_out,
    WhisperRunInfo& info,
    const Clock::time_point& total_start) {
    const bool use_gpu = is_whisper_gpu_requested();
    const int gpu_device = get_whisper_gpu_device();
    const bool flash_attn = is_whisper_flash_attn_enabled();
    const int threads = resolve_threads();

    bool init_reused = false;
    double init_ms = 0.0;
    if (!ensure_resident_context(state, model_path, use_gpu, gpu_device, flash_attn, threads, init_reused, init_ms, error_out)) {
        info.argument_excerpt = build_profile_excerpt(use_gpu, gpu_device, flash_attn, threads);
        info.backend_summary = "resident_init_failed";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.exit_code = 1;
        info.gpu_active = false;
        info.cpu_fallback_reported = false;
        info.init_ms = init_ms;
        info.wav_ms = wav_ms;
        info.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - total_start).count();
        info.timing_excerpt = summarize_timings(init_ms, wav_ms, 0.0, 0.0, info.total_ms);
        pipeline_debug::log("whisper", "mode=in_process_resident failure=resident_init_failed message=" + error_out, true);
        return false;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.translate = false;
    wparams.no_timestamps = true;
    wparams.single_segment = true;
    wparams.language = "en";
    wparams.n_threads = threads;
    wparams.prompt_tokens = nullptr;
    wparams.prompt_n_tokens = 0;
    wparams.temperature_inc = 0.0f;
    if constexpr (has_no_context_field<whisper_full_params>::value) {
        wparams.no_context = true;
    }

    const auto infer_start = Clock::now();
    const int infer_rc = whisper_full(state.ctx, wparams, pcm, pcm_sample_count);
    const double infer_ms = std::chrono::duration<double, std::milli>(Clock::now() - infer_start).count();

    if (infer_rc != 0) {
        error_out = "whisper_full failed with status: " + std::to_string(infer_rc);
        info.argument_excerpt = build_profile_excerpt(use_gpu, gpu_device, flash_attn, threads);
        info.backend_summary = "infer_failed";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.exit_code = 1;
        info.gpu_active = state.use_gpu;
        info.cpu_fallback_reported = false;
        info.init_ms = init_ms;
        info.wav_ms = wav_ms;
        info.infer_ms = infer_ms;
        info.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - total_start).count();
        info.timing_excerpt = summarize_timings(init_ms, wav_ms, infer_ms, 0.0, info.total_ms);
        pipeline_debug::log("whisper", "mode=in_process_resident failure=infer_failed message=" + error_out, true);
        return false;
    }

    const auto extract_start = Clock::now();
    const int n_segments = whisper_full_n_segments(state.ctx);
    std::string transcript;
    for (int i = 0; i < n_segments; ++i) {
        if (const char* segment = whisper_full_get_segment_text(state.ctx, i)) {
            transcript += segment;
        }
    }
    text_out = trim_copy(transcript);
    const double extract_ms = std::chrono::duration<double, std::milli>(Clock::now() - extract_start).count();
    const double total_ms = std::chrono::duration<double, std::milli>(Clock::now() - total_start).count();

    info.argument_excerpt = build_profile_excerpt(use_gpu, gpu_device, flash_attn, threads);
    info.stdout_excerpt.clear();
    info.exit_code = 0;
    info.gpu_active = state.use_gpu;
    info.cpu_fallback_reported = false;
    info.backend_summary = state.use_gpu ? "in_process_gpu" : "in_process_cpu";
    info.init_ms = init_ms;
    info.wav_ms = wav_ms;
    info.infer_ms = infer_ms;
    info.extract_ms = extract_ms;
    info.total_ms = total_ms;
    info.timing_excerpt = summarize_timings(init_ms, wav_ms, infer_ms, extract_ms, total_ms);

    if (text_out.empty()) {
        error_out = "Whisper returned empty transcript output.";
        info.backend_summary = "empty_transcript";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.exit_code = 1;
        pipeline_debug::log("whisper", "mode=in_process_resident failure=empty_transcript", true);
        return false;
    }

    info.stderr_excerpt.clear();

    std::ostringstream log_line;
    log_line << "mode=in_process_resident"
             << " backend=" << info.backend_summary
             << " init_reused=" << (init_reused ? "true" : "false")
             << " wav_ms=" << wav_ms
             << " infer_ms=" << infer_ms
             << " segments=" << n_segments
             << " chars=" << text_out.size();
    pipeline_debug::log("whisper", log_line.str());

    return true;
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
    info.resolved_whisper_executable = "in_process_resident";

    const auto total_start = Clock::now();

    std::filesystem::path model_path;
    if (!resolve_transcribe_inputs(audio_path, model_path, error_out)) {
        info.resolved_model_path = model_path.string();
        info.backend_summary = model_path.empty() ? "input_missing" : "model_missing";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.exit_code = 1;
        pipeline_debug::log("whisper", "mode=in_process_resident failure=" + info.backend_summary + " message=" + error_out, true);
        return false;
    }

    info.resolved_model_path = model_path.string();

    const auto wav_start = Clock::now();
    WavData wav_data;
    if (!load_wav_pcm16_mono_16khz(audio_path, wav_data, error_out)) {
        info.backend_summary = "wav_parse_failed";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.argument_excerpt = build_profile_excerpt(info.gpu_requested, get_whisper_gpu_device(), is_whisper_flash_attn_enabled(), resolve_threads());
        info.exit_code = 1;
        info.timing_excerpt = summarize_timings(0.0, std::chrono::duration<double, std::milli>(Clock::now() - wav_start).count(), 0.0, 0.0,
                                                std::chrono::duration<double, std::milli>(Clock::now() - total_start).count());
        pipeline_debug::log("whisper", "mode=in_process_resident failure=wav_parse_failed message=" + error_out, true);
        return false;
    }
    const double wav_ms = std::chrono::duration<double, std::milli>(Clock::now() - wav_start).count();

    auto& state = resident_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    return transcribe_pcm_locked_with_info(
        state, model_path, wav_data.samples.data(), static_cast<int>(wav_data.samples.size()), wav_ms, text_out, error_out, info, total_start);
}

bool transcribe_pcm_to_string_with_info(
    const float* pcm,
    int pcm_sample_count,
    std::string& text_out,
    std::string& error_out,
    WhisperRunInfo* info_out) {
    text_out.clear();
    error_out.clear();

    WhisperRunInfo local_info{};
    WhisperRunInfo& info = info_out ? *info_out : local_info;
    info = WhisperRunInfo{};
    info.gpu_requested = is_whisper_gpu_requested();
    info.resolved_whisper_executable = "in_process_resident";

    if (!pcm || pcm_sample_count <= 0) {
        error_out = "PCM input is empty.";
        info.backend_summary = "empty_pcm_input";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.exit_code = 1;
        return false;
    }

    const auto model_path = get_whisper_model_path();
    if (!std::filesystem::exists(model_path)) {
        error_out = "Whisper model not found: " + model_path.string();
        info.resolved_model_path = model_path.string();
        info.backend_summary = "model_missing";
        info.stderr_excerpt = clip_excerpt(error_out);
        info.exit_code = 1;
        return false;
    }
    info.resolved_model_path = model_path.string();

    const auto total_start = Clock::now();
    auto& state = resident_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    return transcribe_pcm_locked_with_info(
        state, model_path, pcm, pcm_sample_count, 0.0, text_out, error_out, info, total_start);
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
