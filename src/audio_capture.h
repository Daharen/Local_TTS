#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

class AudioCapture {
public:
    static constexpr int kSampleRate = 16000;

    bool start();
    bool stop();
    bool write_wav(const std::filesystem::path& wav_path) const;
    void get_latest_pcm_ms(int ms, std::vector<float>& out_pcm) const;
    std::size_t get_total_captured_samples() const;
    void cleanup();

private:
#ifdef _WIN32
    static void CALLBACK wave_in_proc(HWAVEIN hwi, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2);
    void on_wave_data(WAVEHDR* header);

    HWAVEIN wave_in_ = nullptr;
    std::vector<WAVEHDR> headers_;
    std::vector<std::vector<char>> buffers_;
#endif

    mutable std::mutex mutex_;
    std::vector<int16_t> samples_;
    std::vector<float> rolling_pcm_;
    std::size_t rolling_max_samples_ = static_cast<std::size_t>(kSampleRate) * 30;
    bool running_ = false;
};
