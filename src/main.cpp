#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include <cstdio>
#include <cmath>

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr float TONE_HZ = 440.0f;
static constexpr float AMPLITUDE = 20000.0f;
static constexpr uint32_t SAMPLES_PER_BUFFER = 256;

static audio_buffer_pool_t *init_audio() {
    static audio_format_t audio_format = {
        .sample_freq = SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static audio_buffer_format_t producer_format = {
        .format = &audio_format,
        .sample_stride = 4, // 2 channels * 2 bytes
    };

    audio_buffer_pool_t *producer_pool =
        audio_new_producer_pool(&producer_format, 3, SAMPLES_PER_BUFFER);

    audio_i2s_config_t config = {
        .data_pin = PICO_AUDIO_I2S_DATA_PIN,
        .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
        .dma_channel = 0,
        .pio_sm = 0,
    };

    const audio_format_t *output_format = audio_i2s_setup(&audio_format, &config);
    if (!output_format) {
        panic("audio_i2s_setup failed");
    }

    bool ok = audio_i2s_connect(producer_pool);
    if (!ok) {
        panic("audio_i2s_connect failed");
    }

    audio_i2s_set_enabled(true);
    return producer_pool;
}

int main() {
    stdio_init_all();

    audio_buffer_pool_t *ap = init_audio();

    uint32_t phase = 0;

    while (true) {
        audio_buffer_t *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *)buffer->buffer->bytes;

        for (uint32_t i = 0; i < buffer->max_sample_count; i++) {
            float t = (float)phase / (float)SAMPLE_RATE;
            int16_t sample = (int16_t)(AMPLITUDE * sinf(2.0f * (float)M_PI * TONE_HZ * t));
            samples[i * 2 + 0] = sample; // left
            samples[i * 2 + 1] = sample; // right

            phase++;
            if (phase >= SAMPLE_RATE) {
                phase -= SAMPLE_RATE;
            }
        }

        buffer->sample_count = buffer->max_sample_count;
        give_audio_buffer(ap, buffer);
    }

    return 0;
}
