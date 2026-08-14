#pragma once

#include <cstdint>

// AY-3-8910/YM2149 instrument format -- P1 scope only (chip.md §12.1's P0
// primitives, now an engine). Deliberately static, no frame table: SID's
// own P1 had "no instrument system yet... note-on uses one fixed default
// patch" before P3 built the frame VM (chip.md §14b.1) -- this is the same
// shape of first step, just already grown to a handful of hand-authored
// patches instead of one, since the format itself needed *something* to
// exercise the mixer/envelope combinations chip.md §12's table calls out.
//
// A per-frame table (arpeggio via wave-table note offsets, volume/mixer
// envelopes the way real AY tracker instruments fake ADSR in software) is
// real, wanted, and not built here -- AY-P2's job, mirroring SID's own
// P1 -> P3 gap.
//
// Real AY hardware has no gate/release concept at the chip level at all --
// a "note off" is a software fiction every tracker invents for itself. This
// format doesn't invent one yet either: chip's audio_engine.cpp mutes a
// voice's output the instant its MIDI gate goes false, envelope-driven or
// not. That is an abrupt, real limitation (no release tail), not a bug --
// AY-P2's frame table is where a real release ramp would live.
struct AyInstrument {
    uint8_t  volume;           // 0-15, used when use_envelope is false
    bool     use_envelope;      // true: envelope_level drives output instead of volume
    uint16_t envelope_period;   // 16-bit register units
    uint8_t  envelope_shape;    // 4-bit raw code (ay_envelope.h's AY_ENVELOPE_SEGMENTS index) --
                                 // chip.md §12's "buzzer bass" is this: a continuing shape
                                 // (e.g. 8 or 10) at audio rate, not silence between notes
    bool     tone_on;           // mixer: is the tone generator part of this instrument
    bool     noise_on;          // mixer: is the noise generator part of this instrument
    uint8_t  noise_period;      // 5-bit, used when noise_on is true
};
