#include "tusb.h"
#include <string.h>

// --- Device Descriptor ---

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,   // Raspberry Pi
    .idProduct          = 0x000A,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// --- Configuration Descriptor ---

enum { ITF_NUM_MIDI = 0, ITF_NUM_MIDI_STREAMING, ITF_NUM_TOTAL };

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, 0x01, 0x81, 64)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// --- String Descriptors ---

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: Language (English)
    "T00T",                       // 1: Manufacturer
    "T00T Synthesizer",           // 2: Product
    "000001"                      // 3: Serial
};

#define STRING_DESC_COUNT (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))

static uint16_t desc_str_buf[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&desc_str_buf[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= STRING_DESC_COUNT) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str_buf[1 + i] = str[i];
        }
    }

    desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str_buf;
}
