#include "encoder_nav.h"

#include "hardware/gpio.h"

// --- Pins (from the board header) ---
#ifndef ENCODER_CLK_PIN
#error "ENCODER_*_PIN must be defined by the board header (HAS_ENCODER board only)"
#endif

static constexpr uint32_t SW_DEBOUNCE_THRESHOLD = 10;  // 10ms at 1ms tick, matches controller.cpp

// Synthetic Sensor-event ids fed through ui_command_shaping.h -- CW/CCW
// detents and the SW press are three independent "buttons" as far as
// Shaping is concerned, each wired to its own UiCommand.
enum EncoderSource : uint8_t { ENC_CW = 0, ENC_CCW = 1, ENC_SW = 2 };

// Quarter-step quadrature transition table, indexed by (old_state << 2 |
// new_state) where state = (CLK << 1) | DT: +1/-1 for a valid single-step
// transition between adjacent Gray-code states, 0 for a repeat or an
// invalid (skipped/bounced) transition.
static const int8_t kQuadTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

static PageCursor cursor;
static UiNavConfig cfg_cw, cfg_ccw, cfg_sw;

static uint8_t quad_state;
static int8_t quad_accum;

static uint8_t sw_counter;
static bool sw_debounced;

void encoder_nav_init(uint8_t page_count, uint8_t performance_index) {
    gpio_init(ENCODER_CLK_PIN);
    gpio_set_dir(ENCODER_CLK_PIN, GPIO_IN);
    gpio_pull_up(ENCODER_CLK_PIN);

    gpio_init(ENCODER_DT_PIN);
    gpio_set_dir(ENCODER_DT_PIN, GPIO_IN);
    gpio_pull_up(ENCODER_DT_PIN);

    gpio_init(ENCODER_SW_PIN);
    gpio_set_dir(ENCODER_SW_PIN, GPIO_IN);
    gpio_pull_up(ENCODER_SW_PIN);

    cfg_cw = { { UiCommand::PLUS }, page_count, performance_index };
    cfg_ccw = { { UiCommand::MINUS }, page_count, performance_index };
    cfg_sw = { { UiCommand::EXIT }, page_count, performance_index };

    quad_state = (uint8_t)((gpio_get(ENCODER_CLK_PIN) << 1) | gpio_get(ENCODER_DT_PIN));
    quad_accum = 0;

    sw_counter = 0;
    sw_debounced = false;  // active-low (pulled up); false = released
}

void encoder_nav_tick() {
    uint8_t new_state = (uint8_t)((gpio_get(ENCODER_CLK_PIN) << 1) | gpio_get(ENCODER_DT_PIN));
    int8_t step = kQuadTable[(quad_state << 2) | new_state];
    quad_state = new_state;

    if (step != 0) {
        quad_accum += step;
        if (quad_accum >= 4) {
            quad_accum = 0;
            SensorEvent ev = sensor_event_button(ENC_CW, true);
            ui_nav_consumer_feed(cursor, ev, cfg_cw);
        } else if (quad_accum <= -4) {
            quad_accum = 0;
            SensorEvent ev = sensor_event_button(ENC_CCW, true);
            ui_nav_consumer_feed(cursor, ev, cfg_ccw);
        }
    }

    // SW is active-low (pulled up): pressed == gpio reads 0.
    bool raw_pressed = !gpio_get(ENCODER_SW_PIN);
    if (raw_pressed) {
        if (sw_counter < SW_DEBOUNCE_THRESHOLD) sw_counter++;
    } else {
        if (sw_counter > 0) sw_counter--;
    }

    bool new_sw = sw_debounced;
    if (sw_counter >= SW_DEBOUNCE_THRESHOLD) {
        new_sw = true;
    } else if (sw_counter == 0) {
        new_sw = false;
    }

    if (new_sw != sw_debounced) {
        sw_debounced = new_sw;
        SensorEvent ev = sensor_event_button(ENC_SW, new_sw);
        ui_nav_consumer_feed(cursor, ev, cfg_sw);
    }
}

uint8_t encoder_nav_page_index() {
    return cursor.index;
}
