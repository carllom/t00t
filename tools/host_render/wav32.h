#pragma once

// WAV writers for the chip module's F0 comparison (chip.md §11.1). Shared by
// tools/sid_ref/resid_render.cpp and tools/host_render/render_sid.cpp -- both
// sides of the comparison must write byte-identical container formats or
// sid_compare.py is comparing readers as much as engines.
//
// The analysis path is 32-bit float, deliberately: reSID's output and
// src/chip/'s fixed-point contract are on different scales (reSID is a
// 16-bit PCM synthesis, t00t's voice_output units are (waveform_dac -
// wave_zero) * envelope_dac, §sid_voice.h), so quantising either to a shared
// PCM16 range would bake in a guessed headroom constant. Float sidesteps it:
// each renderer writes its own natural scale and sid_compare.py normalises.
// Same reasoning, same file shape, as the FM module's tools/fm_ref/wav32.h.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// IEEE float32 mono/stereo WAV (format tag 3). Includes the `fact` chunk that
// non-PCM WAV requires.
inline bool write_wav_f32(const std::string &path, const std::vector<float> &samples,
                          uint32_t sample_rate, uint16_t channels = 1) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;

    uint32_t data_bytes   = (uint32_t)(samples.size() * sizeof(float));
    uint16_t bits         = 32;
    uint16_t block_align  = (uint16_t)(channels * sizeof(float));
    uint32_t byte_rate    = sample_rate * block_align;
    uint32_t fmt_size     = 16;
    uint16_t audio_format = 3;  // IEEE float
    uint32_t fact_size    = 4;
    uint32_t fact_samples = (uint32_t)(samples.size() / channels);
    uint32_t riff_size    = 4 + (8 + fmt_size) + (8 + fact_size) + (8 + data_bytes);

    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);  fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);  fwrite(&bits, 2, 1, f);
    fwrite("fact", 1, 4, f); fwrite(&fact_size, 4, 1, f); fwrite(&fact_samples, 4, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(samples.data(), sizeof(float), samples.size(), f);

    fclose(f);
    return true;
}

// Listenable companion: peak-normalised to -1 dBFS PCM16. For ears at a
// checkpoint, never for analysis -- the per-file normalisation destroys
// exactly the absolute-level information sid_compare.py's level_gap metric
// reports.
inline bool write_wav_pcm16_normalized(const std::string &path, const std::vector<float> &samples,
                                       uint32_t sample_rate, uint16_t channels = 1) {
    float peak = 0.0f;
    for (float s : samples) { float m = std::fabs(s); if (m > peak) peak = m; }
    const float target = 0.891f;  // -1 dBFS
    float scale = peak > 0.0f ? target / peak : 0.0f;

    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        float v = samples[i] * scale * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = (int16_t)std::lrintf(v);
    }

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t data_bytes  = (uint32_t)(pcm.size() * sizeof(int16_t));
    uint16_t bits        = 16;
    uint16_t block_align = (uint16_t)(channels * sizeof(int16_t));
    uint32_t byte_rate   = sample_rate * block_align;
    uint32_t fmt_size    = 16;
    uint16_t audio_format = 1;
    uint32_t riff_size   = 36 + data_bytes;

    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);  fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);  fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
    fclose(f);
    return true;
}
