#include "audio_capture.h"

#include <cstring>
#include <fstream>

namespace {

void write_u16(std::ofstream& out, uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u32(std::ofstream& out, uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

}  // namespace

bool AudioCapture::start() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return true;
    }

    samples_.clear();
    buffers_.clear();
    headers_.clear();

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    const MMRESULT open_result = waveInOpen(
        &wave_in_,
        WAVE_MAPPER,
        &format,
        reinterpret_cast<DWORD_PTR>(&AudioCapture::wave_in_proc),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);

    if (open_result != MMSYSERR_NOERROR) {
        wave_in_ = nullptr;
        return false;
    }

    constexpr std::size_t kBufferCount = 4;
    constexpr std::size_t kBufferBytes = 4096;

    buffers_.resize(kBufferCount);
    headers_.resize(kBufferCount);

    for (std::size_t i = 0; i < kBufferCount; ++i) {
        buffers_[i].resize(kBufferBytes);
        WAVEHDR& header = headers_[i];
        std::memset(&header, 0, sizeof(header));
        header.lpData = buffers_[i].data();
        header.dwBufferLength = static_cast<DWORD>(buffers_[i].size());

        if (waveInPrepareHeader(wave_in_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            cleanup();
            return false;
        }
        if (waveInAddBuffer(wave_in_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            cleanup();
            return false;
        }
    }

    if (waveInStart(wave_in_) != MMSYSERR_NOERROR) {
        cleanup();
        return false;
    }

    running_ = true;
    return true;
#else
    return false;
#endif
}

bool AudioCapture::stop() {
#ifdef _WIN32
    HWAVEIN handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return true;
        }
        handle = wave_in_;
        running_ = false;
    }

    waveInStop(handle);
    waveInReset(handle);

    for (auto& header : headers_) {
        waveInUnprepareHeader(handle, &header, sizeof(header));
    }

    waveInClose(handle);

    std::lock_guard<std::mutex> lock(mutex_);
    wave_in_ = nullptr;
    headers_.clear();
    buffers_.clear();
    return true;
#else
    return false;
#endif
}

bool AudioCapture::write_wav(const std::filesystem::path& wav_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::create_directories(wav_path.parent_path());

    const uint32_t sample_rate = 16000;
    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t data_size = static_cast<uint32_t>(samples_.size() * sizeof(int16_t));
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t chunk_size = 36 + data_size;

    std::ofstream out(wav_path, std::ios::binary);
    if (!out) {
        return false;
    }

    out.write("RIFF", 4);
    write_u32(out, chunk_size);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    write_u32(out, 16);
    write_u16(out, 1);
    write_u16(out, channels);
    write_u32(out, sample_rate);
    write_u32(out, byte_rate);
    write_u16(out, block_align);
    write_u16(out, bits_per_sample);

    out.write("data", 4);
    write_u32(out, data_size);
    if (data_size > 0) {
        out.write(reinterpret_cast<const char*>(samples_.data()), data_size);
    }

    return static_cast<bool>(out);
}

void AudioCapture::cleanup() {
#ifdef _WIN32
    HWAVEIN handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handle = wave_in_;
        running_ = false;
    }

    if (handle) {
        waveInStop(handle);
        waveInReset(handle);
        for (auto& header : headers_) {
            waveInUnprepareHeader(handle, &header, sizeof(header));
        }
        waveInClose(handle);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    wave_in_ = nullptr;
    headers_.clear();
    buffers_.clear();
#endif
}

#ifdef _WIN32
void CALLBACK AudioCapture::wave_in_proc(HWAVEIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (msg != WIM_DATA || instance == 0 || param1 == 0) {
        return;
    }
    auto* capture = reinterpret_cast<AudioCapture*>(instance);
    capture->on_wave_data(reinterpret_cast<WAVEHDR*>(param1));
}

void AudioCapture::on_wave_data(WAVEHDR* header) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !wave_in_ || !header || header->dwBytesRecorded == 0) {
        return;
    }

    const auto* begin = reinterpret_cast<const int16_t*>(header->lpData);
    const auto sample_count = static_cast<std::size_t>(header->dwBytesRecorded / sizeof(int16_t));
    samples_.insert(samples_.end(), begin, begin + sample_count);

    header->dwBytesRecorded = 0;
    waveInAddBuffer(wave_in_, header, sizeof(*header));
}
#endif
