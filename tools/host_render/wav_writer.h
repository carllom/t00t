#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal mono/stereo PCM16 WAV writer. No dependency on the device build —
// this is host-only tooling (see tools/host_render).
inline bool write_wav_pcm16(const std::string &path, const std::vector<int16_t> &samples,
                             uint32_t sample_rate, uint16_t channels = 1) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;

    uint32_t data_bytes  = (uint32_t)(samples.size() * sizeof(int16_t));
    uint32_t byte_rate   = sample_rate * channels * sizeof(int16_t);
    uint16_t block_align = (uint16_t)(channels * sizeof(int16_t));
    uint32_t riff_size   = 36 + data_bytes;
    uint32_t fmt_size    = 16;
    uint16_t audio_format = 1;  // PCM
    uint16_t bits_per_sample = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    fwrite(samples.data(), sizeof(int16_t), samples.size(), f);

    fclose(f);
    return true;
}
