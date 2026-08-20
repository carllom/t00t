#include "midi_dispatch.h"

// Per-channel bank-select state (CC0 MSB / CC32 LSB) -- module-agnostic,
// always linked regardless of which engine is compiled in.
static constexpr uint8_t NUM_CHANNELS = 16;
static uint8_t channel_bank_msb[NUM_CHANNELS];
static uint8_t channel_bank_lsb[NUM_CHANNELS];

void midi_bank_select_init() {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_bank_msb[ch] = 0;
        channel_bank_lsb[ch] = 0;
    }
}

void midi_bank_select_msb(uint8_t channel, uint8_t value) {
    if (channel < NUM_CHANNELS) channel_bank_msb[channel] = value;
}

void midi_bank_select_lsb(uint8_t channel, uint8_t value) {
    if (channel < NUM_CHANNELS) channel_bank_lsb[channel] = value;
}

uint8_t midi_channel_bank_msb(uint8_t channel) {
    return channel < NUM_CHANNELS ? channel_bank_msb[channel] : 0;
}

uint8_t midi_channel_bank_lsb(uint8_t channel) {
    return channel < NUM_CHANNELS ? channel_bank_lsb[channel] : 0;
}
