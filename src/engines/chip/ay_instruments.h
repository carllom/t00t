#pragma once

#include "ay_instrument.h"

// Hand-authored AY instruments (module_chip.md §12.1/§12.2/§12.3) -- one
// documented feature each. LEAD/BUZZ_BASS/NOISE_PERC are a static trio;
// ARP and PLUCK use the frame table; LEAD_YM demonstrates YM2149 vs
// AY8910.
//
// Not generated: a chipgen.py-style text format is premature for six
// static patches. Revisit once real-world authoring volume justifies the
// tooling.

enum AyInstrumentId : uint8_t {
    AY_INS_LEAD,        // tone only, static volume -- plain square-wave lead
    AY_INS_BUZZ_BASS,   // tone + envelope shape 8 (slide_down/slide_down --
                         // a repeating sawtooth decay), period tuned for
                         // a ~50 Hz buzz
    AY_INS_NOISE_PERC,   // noise only + envelope shape 9 (slide_down/
                         // hold_bottom -- one decay then silent): the
                         // envelope's natural one-shot shape stands in for
                         // a percussive release with no frame table needed
    AY_INS_ARP,          // tone table: major-triad arpeggio (0, +4, +7),
                         // plus light vibrato -- same "one row = one
                         // frame, xN to hold" authoring convention SID's
                         // own tables use.
    AY_INS_PLUCK,        // tone + a software volume table (SweepTable, the
                         // real AY answer to "no hardware release"): quick
                         // ramp down to silence, then gate_off_timer ends
                         // the note outright -- no hardware envelope at all
    AY_INS_LEAD_YM,      // AY_INS_LEAD, byte-for-byte, except model --
                         // demonstrates the one real, documented AY8910 vs
                         // YM2149 difference (ay_envelope.h's DAC tables)
    AY_INSTRUMENT_COUNT,
};

// module_chip.md §12.4: display.cpp names, same role chipgen.py's generated
// INSTRUMENT_NAMES[] plays for SID -- hand-written here since this table
// is hand-written too (no generator to emit it from yet, ay_instruments.h's
// own header comment).
static const char *const AY_INSTRUMENT_NAMES[AY_INSTRUMENT_COUNT] = {
    "LEAD", "BUZZ_BASS", "NOISE_PERC", "ARP", "PLUCK", "LEAD_YM",
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
        /*model*/ AY_MODEL_AY8910,
        /*initial_volume*/ 12, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
    // AY_INS_BUZZ_BASS
    {
        /*model*/ AY_MODEL_AY8910,
        /*initial_volume*/ 0, /*use_envelope*/ true, /*envelope_period*/ 138, /*envelope_shape*/ 8,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
    // AY_INS_NOISE_PERC
    {
        /*model*/ AY_MODEL_AY8910,
        /*initial_volume*/ 0, /*use_envelope*/ true, /*envelope_period*/ 1400, /*envelope_shape*/ 9,
        /*tone_on*/ false, /*noise_on*/ true, /*noise_period*/ 12,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
    // AY_INS_ARP
    {
        /*model*/ AY_MODEL_AY8910,
        /*initial_volume*/ 12, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { AY_ARP_TONE, 8, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 20, /*vibrato_speed*/ 40, /*vibrato_delay*/ 30, /*gate_off_timer*/ 0,
    },
    // AY_INS_PLUCK
    {
        /*model*/ AY_MODEL_AY8910,
        /*initial_volume*/ 15, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { AY_PLUCK_VOLUME, 2, 1 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 60,
    },
    // AY_INS_LEAD_YM -- AY_INS_LEAD with model swapped, nothing else changed
    {
        /*model*/ AY_MODEL_YM2149,
        /*initial_volume*/ 12, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
        /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0,
        { nullptr, 0, 0 }, { nullptr, 0, 0 },
        /*vibrato_depth*/ 0, 0, 0, /*gate_off_timer*/ 0,
    },
};
