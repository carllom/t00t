#pragma once

#include "../engine_base.h"
#include "midi_controller.h"
#include "midi_dispatch.h"
#include "midi_parser.h"
#include <cstdint>

// The full parse-and-dispatch loop for a standard, "one voice per note"
// module: feeds raw bytes through midi_parser.h's shared MidiParser
// instance, routes each parsed message through midi_dispatch.h's generic
// helpers (updating midi_controller.h's shared MidiUiState where a
// Handler can't -- see the MIDI_PITCH_BEND case below), and commits the
// shadow block if anything changed. A module's own top-level
// midi_controller_process() (required per-engine by the build's file
// selection, see CMakeLists.txt) is a one-line call into this -- only the
// table and per-voice/per-channel state stay the caller's own.
//
// Deliberately does not resolve a voice for NOTE_ON/OFF, or touch
// voice_alloc.h at all: voice resolution is the Voice Allocation
// Interface's job, reached from inside a module's own NOTE Handler, never
// interleaved in parsing/dispatch (CONTEXT.md's "Voice Allocation
// Interface" entry). That's also why this loop needs no per-note/per-voice
// tracking arrays passed in -- a module keeps those entirely to itself.
//
// Separate from midi_dispatch.h because ParamExchangeT/VoiceParamBlockT
// (engine_base.h) need MAX_VOICES/FILTER_BUS_COUNT already defined by
// whichever engine.h the caller included first -- fine for a per-engine
// input_subsystem.cpp, not for midi_dispatch.cpp's own module-agnostic,
// always-linked bank-select state, which has no such engine in scope.
template <typename VoiceParams, uint32_t N>
inline void midi_controller_process_generic(
        const uint8_t *data, uint32_t len, ParamExchangeT<VoiceParams> *params,
        const InputMapEntryT<VoiceParamBlockT<VoiceParams>> (&table)[N]) {
    if (len == 0) return;

    bool changed = false;
    VoiceParamBlockT<VoiceParams> &shadow = params->shadow();
    shadow = params->active();

    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!midi_parser.feed(data[i], ev)) continue;

        switch (ev.type) {
            case MIDI_NOTE_ON:
                midi_dispatch_note(shadow, table, ev.data1, ev.channel, ev.data2, -1, true);
                changed = true;
                break;
            case MIDI_NOTE_OFF:
                midi_dispatch_note(shadow, table, ev.data1, ev.channel, 0, -1, false);
                changed = true;
                break;
            case MIDI_CC:
                switch (ev.data1) {
                    case 0:  midi_bank_select_msb(ev.channel, ev.data2); break;
                    case 32: midi_bank_select_lsb(ev.channel, ev.data2); break;
                    default:
                        midi_dispatch_cc(shadow, table, ev.channel, ev.data1, ev.data2);
                        changed = true;
                        break;
                }
                break;
            case MIDI_PITCH_BEND: {
                // midi_dispatch_pitch_bend() converts bend14 -> a phase_inc
                // ratio before a Handler ever sees it, so the raw signed
                // offset the display wants is captured here instead.
                uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                midi_dispatch_pitch_bend(shadow, table, ev.channel, bend14);
                ui_state.bend = (int16_t)((int)bend14 - 8192);
                ui_state.last_channel = ev.channel;
                changed = true;
                break;
            }
            case MIDI_PROGRAM_CHANGE:
                midi_dispatch_program_change(shadow, table, ev.channel, ev.data1);
                break;
            case MIDI_START:
            case MIDI_CONTINUE:
            case MIDI_STOP:
                // Transport, like Configuration above, is Core-0-local by
                // nature (a module's own transport state, not a shadow
                // VoiceParams write) -- no params->commit() needed for it.
                midi_dispatch_transport(shadow, table, (uint8_t)ev.type);
                break;
            default: break;
        }
    }

    if (changed) {
        params->commit();
    }
}
