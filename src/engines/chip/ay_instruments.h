#pragma once

#include "ay_instrument.h"

// Hand-authored AY instruments (chip.md §12.1/AY-P1) -- three patches, one
// per mixer/envelope combination chip.md §12's own table calls for
// exercising, same "one documented feature each" shape SID's P3 instruments
// used (chip.md §14d.5).
//
// Not generated: AY-P2's frame table (once it exists) is the natural home
// for a chipgen.py-style text format, same as SID's chip_instruments.txt --
// premature for three static patches.

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
    AY_INSTRUMENT_COUNT,
};

static const AyInstrument AY_INSTRUMENTS[AY_INSTRUMENT_COUNT] = {
    // AY_INS_LEAD
    { /*volume*/ 12, /*use_envelope*/ false, /*envelope_period*/ 0, /*envelope_shape*/ 0,
      /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0 },
    // AY_INS_BUZZ_BASS
    { /*volume*/ 0, /*use_envelope*/ true, /*envelope_period*/ 138, /*envelope_shape*/ 8,
      /*tone_on*/ true, /*noise_on*/ false, /*noise_period*/ 0 },
    // AY_INS_NOISE_PERC
    { /*volume*/ 0, /*use_envelope*/ true, /*envelope_period*/ 1400, /*envelope_shape*/ 9,
      /*tone_on*/ false, /*noise_on*/ true, /*noise_period*/ 12 },
};
