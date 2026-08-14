#pragma once

#include "ay_instrument.h"

// Hand-authored AY instruments (chip.md §12.1/§12.2) -- one documented
// feature each, same pattern SID's own P3 instruments used (chip.md
// §14d.5). LEAD/BUZZ_BASS/NOISE_PERC are P1's hardware-verified static
// trio, unchanged; ARP and PLUCK are P2's frame-table additions.
//
// Not generated: same reasoning as before -- a chipgen.py-style text
// format is premature for five static patches. Revisit once real-world
// authoring volume justifies the tooling, same threshold SID crossed at P4.

enum AyInstrumentId : uint8_t {
    AY_INS_LEAD,        // tone only, static volume -- plain square-wave lead
    AY_INS_BUZZ_BASS,   // tone + envelope shape 8 (slide_down/slide_down --
                         // a repeating sawtooth decay): chip.md §12's own
                         // "envelope generator at audio rate as a buzzer
                         // bass source", period tuned for a ~50 Hz buzz
    AY_INS_NOISE_PERC,   // noise only + envelope shape 9 (slide_down/
                         // hold_bottom -- one decay then silent): the
                         // envelope's natural one-shot shape stands in for
                         // a percussive release with no frame table needed
    AY_INS_ARP,          // tone table: major-triad arpeggio (0, +4, +7),
                         // plus light vibrato -- AY-P2's tone table and
                         // vibrato, same "one row = one frame, xN to hold"
                         // authoring convention SID's own tables use
    AY_INS_PLUCK,        // tone + a software volume table (SweepTable, the
                         // real AY answer to "no hardware release"): quick
                         // ramp down to silence, then gate_off_timer ends
                         // the note outright -- no hardware envelope at all
    AY_INSTRUMENT_COUNT,
};

static const AyToneRow AY_ARP_TONE[] = {
    { 0 }, { 0 },
    { 4 }, { 4 },
    { 7 }, { 7 },
    { 12 }, { 12 },
};

static const SweepRow AY_PLUCK_VOLUME[] = {
    { -3, 4 },    // 15 -> 3 over 4 frames: the fast part of a pluck's decay
    { 0, 250 },   // hold near-silent until gate_off_timer ends the note
};

static const AyInstrument AY_INSTRUMENTS[AY_INSTRUMENT_COUNT] = {
    // AY_INS_LEAD
    {
        /*initial_volume*/ 12, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
    // AY_INS_BUZZ_BASS
    {
        /*initial_volume*/ 0, /*use_envelope*/ true, /*envelope_period*/ 138, /*envelope_shape*/ 8,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
    // AY_INS_NOISE_PERC
    {
        /*initial_volume*/ 0, /*use_envelope*/ true, /*envelope_period*/ 1400, /*envelope_shape*/ 9,
        /*tone_on*/ false, /*noise_on*/ true, /*noise_period*/ 12,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
    // AY_INS_ARP
    {
        /*initial_volume*/ 12, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { AY_ARP_TONE, 8, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 20, /*vibrato_speed*/ 40, /*vibrato_delay*/ 30, /*gate_off_timer*/ 0,
    },
    // AY_INS_PLUCK
    {
        /*initial_volume*/ 15, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { AY_PLUCK_VOLUME, 2, 1 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 60,
    },
};
