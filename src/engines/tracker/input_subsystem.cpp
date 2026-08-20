#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "player_task.h"

// tracker's Input subsystem: no live-note traffic in this engine at all
// (module_tracker.md) -- the module plays itself (player_task.cpp
// auto-plays on boot), so MIDI here drives transport/seek rather than
// notes. kMappingTable has no NOTE/MODIFIER entries; MIDI note on/off and
// any CC silently no-op through the Router, same as any unmapped id.
//
// Program Change is this module's Configuration input, same as every
// other module using the Router, but its own meaning: seeking to an order
// rather than selecting a preset. CONFIGURATION's `value.index` field
// means whatever a module's own Handler decides it means -- here, the
// order index to seek to.

static void set_start(VoiceParamBlock &, const InputValue &) {  // rewind + play -- conventional MIDI Start semantics
    tracker_transport_seek(0);
    tracker_transport_play();
}

static void set_continue(VoiceParamBlock &, const InputValue &) {  // resume from wherever transport_stop() left it
    tracker_transport_play();
}

static void set_stop(VoiceParamBlock &, const InputValue &) {
    tracker_transport_stop();
}

static void set_seek(VoiceParamBlock &, const InputValue &value) {
    tracker_transport_seek(value.index);
    ui_state.program = value.index;
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::TRANSPORT,
    InputCategory::CONFIGURATION,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category                  id_low                  id_high                 channel  fixed_vel  setter
    { InputCategory::TRANSPORT,     MIDI_START,             MIDI_START,             0xFF,   0,       set_start },
    { InputCategory::TRANSPORT,     MIDI_CONTINUE,          MIDI_CONTINUE,          0xFF,   0,       set_continue },
    { InputCategory::TRANSPORT,     MIDI_STOP,              MIDI_STOP,              0xFF,   0,       set_stop },
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF,   0,       set_seek },
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "tracker mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    ui_state = { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    midi_controller_process_generic(data, len, params, kMappingTable);
}
