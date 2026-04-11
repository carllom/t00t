#include "output.h"
#include "pico/audio_i2s.h"

struct I2SOutput : AudioOutput {
    audio_buffer_pool_t *pool;
    audio_buffer_t *current_buffer;

    bool init() override;
    int16_t *get_buffer(uint32_t *num_samples) override;
    void submit_buffer(uint32_t num_samples) override;
};

bool I2SOutput::init() {
    static audio_format_t audio_format = {
        .sample_freq = SAMPLE_RATE,
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };

    static audio_buffer_format_t producer_format = {
        .format = &audio_format,
        .sample_stride = 4,
    };

    pool = audio_new_producer_pool(&producer_format, 3, SAMPLES_PER_BUFFER);

    audio_i2s_config_t config = {
        .data_pin = PICO_AUDIO_I2S_DATA_PIN,
        .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
        .dma_channel = 0,
        .pio_sm = 0,
    };

    if (!audio_i2s_setup(&audio_format, &config)) {
        return false;
    }

    if (!audio_i2s_connect(pool)) {
        return false;
    }

    audio_i2s_set_enabled(true);
    current_buffer = nullptr;
    return true;
}

int16_t *I2SOutput::get_buffer(uint32_t *num_samples) {
    current_buffer = take_audio_buffer(pool, true);
    *num_samples = current_buffer->max_sample_count;
    return (int16_t *)current_buffer->buffer->bytes;
}

void I2SOutput::submit_buffer(uint32_t num_samples) {
    current_buffer->sample_count = num_samples;
    give_audio_buffer(pool, current_buffer);
    current_buffer = nullptr;
}

static I2SOutput i2s_instance;

AudioOutput *create_i2s_output() {
    return &i2s_instance;
}
