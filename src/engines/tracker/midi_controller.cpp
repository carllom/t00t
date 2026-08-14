#include "midi_controller.h"
#include "midi_parser.h"
#include "player_task.h"

// Tracker transport control (#18): the module plays itself (player_task.cpp
// auto-plays on boot), so MIDI here drives play/stop/seek rather than notes
// -- there's no live-note path in this engine at all (module_tracker.md
// non-goals: "Live editing or pattern entry on-device"). Modeled directly
// on src/engines/groovebox/midi_controller.cpp's MIDI_START/MIDI_STOP
// handling of its own sequencer's seq_clock_running flag: same MidiParser,
// same transport-message vocabulary, driving player_task's transport
// functions instead of a step-sequencer flag.

static MidiParser midi_parser;
static MidiUiState ui_state;

void midi_controller_init() {
    midi_parser.init();
    ui_state = { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *) {
    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!midi_parser.feed(data[i], ev)) continue;

        switch (ev.type) {
            case MIDI_START:  // rewind + play -- conventional MIDI Start semantics
                tracker_transport_seek(0);
                tracker_transport_play();
                break;
            case MIDI_CONTINUE:  // resume from wherever transport_stop() left it
                tracker_transport_play();
                break;
            case MIDI_STOP:
                tracker_transport_stop();
                break;
            case MIDI_PROGRAM_CHANGE:
                tracker_transport_seek(ev.data1);
                ui_state.program = ev.data1;
                break;
            default:
                break;  // no live-note path in this engine
        }
    }
}

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }
