// Host-buildable unit test for src/button_shaping.h -- the SensorEvent ->
// Shaping -> input_dispatch() path a GPIO button takes to reach the same
// Handler a MIDI note-on/off would. Header-only, no hardware dependency --
// same convention as test_input_layer.cpp / test_sensor_event.cpp (test_*()
// functions, an aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").
//
// Deliberately dispatches against a local test table, not subtractive's
// real kMappingTable/set_note (in src/midi/midi_controller.cpp) -- that
// file pulls in pico-sdk-dependent headers and stays verified on real
// hardware instead.

#include "../../src/button_shaping.h"

#include <cstdio>

namespace {

struct TestContext {
    int calls = 0;
    uint8_t last_channel = 0;
    uint8_t last_note = 0;
    uint8_t last_velocity = 0;
    int8_t last_voice = -1;
    bool last_note_on = false;
};

void set_note(TestContext &ctx, const InputValue &v) {
    ctx.calls++;
    ctx.last_channel = v.channel;
    ctx.last_note = v.note;
    ctx.last_velocity = v.velocity;
    ctx.last_voice = v.voice;
    ctx.last_note_on = v.note_on;
}

constexpr InputMapEntryT<TestContext> kTable[] = {
    // category            id_low  id_high  channel  fixed_vel  setter
    { InputCategory::NOTE,  60,     60,      0xFF,    0,         set_note },  // kButton0's note, any channel
    { InputCategory::NOTE,  90,     90,      5,       0,         set_note },  // channel 5 only
};

const ButtonShapingConfig kButton0 = { /* note */ 60, /* channel */ 1, /* fixed_velocity */ 100 };

bool test_press_produces_note_on_with_fixed_velocity() {
    SensorEvent ev = sensor_event_button(0, true);
    InputValue value = shape_button_event(ev, kButton0);

    bool ok = value.channel == 1 && value.note == 60 && value.velocity == 100 && value.note_on;
    printf(ok ? "  OK: press shapes into note-on, channel/note from config, velocity fixed-substituted\n"
              : "  FAIL: pressed button did not shape into the expected Input event\n");
    return ok;
}

bool test_release_produces_note_off_with_zero_velocity() {
    SensorEvent ev = sensor_event_button(0, false);
    InputValue value = shape_button_event(ev, kButton0);

    bool ok = value.channel == 1 && value.note == 60 && value.velocity == 0 && !value.note_on;
    printf(ok ? "  OK: release shapes into note-off, velocity forced to 0\n"
              : "  FAIL: released button did not shape into the expected Input event\n");
    return ok;
}

bool test_fixed_velocity_substitutes_missing_signal() {
    // A bare switch carries no natural velocity -- two different buttons
    // configured with different fixed_velocity values must produce exactly
    // that value, not some derived or shared default.
    ButtonShapingConfig soft = { 60, 0, 40 };
    ButtonShapingConfig loud = { 60, 0, 120 };

    InputValue v_soft = shape_button_event(sensor_event_button(0, true), soft);
    InputValue v_loud = shape_button_event(sensor_event_button(1, true), loud);

    bool ok = v_soft.velocity == 40 && v_loud.velocity == 120;
    printf(ok ? "  OK: Shaping's fixed-value substitution supplies each button's own configured velocity\n"
              : "  FAIL: fixed velocity was not substituted per-button config\n");
    return ok;
}

bool test_shaped_button_event_dispatches_to_handler() {
    TestContext ctx;
    InputValue value = shape_button_event(sensor_event_button(0, true), kButton0);
    input_dispatch(ctx, kTable, InputCategory::NOTE, kButton0.note, value);

    bool ok = ctx.calls == 1 && ctx.last_channel == 1 && ctx.last_note == 60 &&
              ctx.last_velocity == 100 && ctx.last_note_on;
    printf(ok ? "  OK: a Shaped button press reaches the Handler via input_dispatch()\n"
              : "  FAIL: Shaped button press did not reach the Handler correctly\n");
    return ok;
}

bool test_channel_filtered_button_event_never_reaches_handler() {
    // kTable's id-90 entry only admits channel 5; a button configured on a
    // different channel must be dropped before it ever reaches a Handler.
    TestContext ctx;
    ButtonShapingConfig wrong_channel = { 90, 2, 100 };
    InputValue value = shape_button_event(sensor_event_button(0, true), wrong_channel);
    input_dispatch(ctx, kTable, InputCategory::NOTE, wrong_channel.note, value);

    bool ok = ctx.calls == 0;
    printf(ok ? "  OK: a button event on a filtered-out channel never reaches the Handler\n"
              : "  FAIL: a channel-filtered button event incorrectly reached the Handler\n");
    return ok;
}

bool test_button_and_midi_sourced_events_converge() {
    // A button-sourced Input event and an equivalent MIDI-sourced one
    // (built the way midi_controller.cpp already does today: velocity
    // straight off the wire, resolved voice from the allocator) must
    // produce the same Handler call for the same semantic note/channel/
    // velocity -- proving "source-agnostic" is a property of the shape,
    // not just of this one Shaping function.
    TestContext ctx_button, ctx_midi;

    InputValue button_value = shape_button_event(sensor_event_button(0, true), kButton0);
    button_value.voice = 3;
    input_dispatch(ctx_button, kTable, InputCategory::NOTE, kButton0.note, button_value);

    InputValue midi_value{};
    midi_value.channel = kButton0.channel;
    midi_value.note = kButton0.note;
    midi_value.velocity = kButton0.fixed_velocity;  // MIDI note-on already carries a real velocity byte
    midi_value.voice = 3;
    midi_value.note_on = true;
    input_dispatch(ctx_midi, kTable, InputCategory::NOTE, kButton0.note, midi_value);

    bool ok = ctx_button.calls == 1 && ctx_midi.calls == 1 &&
              ctx_button.last_channel == ctx_midi.last_channel &&
              ctx_button.last_note == ctx_midi.last_note &&
              ctx_button.last_velocity == ctx_midi.last_velocity &&
              ctx_button.last_voice == ctx_midi.last_voice &&
              ctx_button.last_note_on == ctx_midi.last_note_on;
    printf(ok ? "  OK: button-sourced and MIDI-sourced Input events converge on an identical Handler call\n"
              : "  FAIL: button-sourced and MIDI-sourced Input events diverged at the Handler\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== button press shapes into note-on ==\n");
    ok = test_press_produces_note_on_with_fixed_velocity() && ok;
    printf("\n== button release shapes into note-off ==\n");
    ok = test_release_produces_note_off_with_zero_velocity() && ok;
    printf("\n== fixed-velocity substitution ==\n");
    ok = test_fixed_velocity_substitutes_missing_signal() && ok;
    printf("\n== Shaped button event reaches the Handler ==\n");
    ok = test_shaped_button_event_dispatches_to_handler() && ok;
    printf("\n== channel-filtered button event is dropped before the Handler ==\n");
    ok = test_channel_filtered_button_event_never_reaches_handler() && ok;
    printf("\n== button-sourced and MIDI-sourced events converge ==\n");
    ok = test_button_and_midi_sourced_events_converge() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
