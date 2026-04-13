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
    bool start();
    bool stop();
    bool write_wav(const std::filesystem::path& wav_path) const;
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
    bool running_ = false;
};
