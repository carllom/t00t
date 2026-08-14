#pragma once

#include "instrument.h"     // SweepRow/SweepTable -- reused verbatim, see below
#include "chip/ay_envelope.h"   // AyModel
#include <cstdint>

// AY-3-8910/YM2149 instrument format. The frame table (module_chip.md
// §12.2) adds arpeggio and software envelope/mixer automation on top of
// the static base below.
//
// A trigger with no tone/volume table rows behaves as a static instrument
// -- initial_volume/use_envelope apply once, nothing steps.
//
// Real AY-3-8910/YM2149 hardware has no gate/release concept at the chip
// level at all -- every tracker's "note off" is a software fiction, and a
// per-frame volume table is how real AY tunes fake one. The `volume`
// table (and `gate_off_timer`, mirroring SID's own) is where a release
// ramp lives.
//
// SweepRow/SweepTable (instrument.h) are reused verbatim for the volume
// table -- same shape as SID's pulse/filter sweeps (a per-frame delta held
// for `duration` frames), and a software volume ramp is exactly that kind
// of ramp, just targeting a 0-15 register instead of a 12-bit pulse width
// or an 11-bit cutoff. No reason to invent a second row type for the same
// stepping logic.

// AyToneTable row: module_chip.md's arpeggio pattern (relative semitone
// offset per frame), simplified from SID's WaveRow -- AY has no waveform
// bits to hold, so there is nothing here to mirror WAVE_HOLD for. Repeat
// by duplicating a row, same authoring convention as chip's hand-written
// SID instruments.
struct AyToneRow {
    int8_t note;   // semitone offset from the played note
};
struct AyToneTable { const AyToneRow *rows; uint8_t len; uint8_t loop; };

struct AyInstrument {
    AyModel  model;              // AY_MODEL_AY8910 or AY_MODEL_YM2149 (ay_envelope.h) --
                                 // selects the DAC table (ay_dac_table()), the one
                                 // real, documented difference between the two chips
                                 // this format models. Per-instrument, not per-song or
                                 // per-build: real tunes were authored for one specific
                                 // chip, so a patch says which it wants, same as it
                                 // already says its own mixer/envelope settings.
    uint8_t  initial_volume;    // 0-15, applied at trigger before the volume
                                 // table's row 0 (if any) takes effect at the
                                 // first frame tick; the whole story if there
                                 // is no volume table
    bool     use_envelope;      // true: envelope_level drives output instead
                                 // of initial_volume/the volume table -- real
                                 // hardware's e_on bit is exclusive with
                                 // manual volume, so this format keeps that:
                                 // a volume table on an envelope-driven
                                 // instrument is simply not read
    uint16_t envelope_period;   // 16-bit register units
    uint8_t  envelope_shape;    // 4-bit raw code (ay_envelope.h's AY_ENVELOPE_SEGMENTS index) --
                                 // module_chip.md §12's "buzzer bass" is this: a continuing shape
                                 // (e.g. 8 or 10) at audio rate, not silence between notes
    bool     tone_on;           // mixer: is the tone generator part of this instrument
    bool     noise_on;          // mixer: is the noise generator part of this instrument
    uint8_t  noise_period;      // 5-bit, used when noise_on is true

    AyToneTable tone;            // arpeggio (module_chip.md §12.2)
    SweepTable  volume;           // software envelope, ignored when use_envelope
                                    // is true (see above); clamped 0-15 same as
                                    // real hardware's volume register

    uint16_t vibrato_depth;      // 0 = off. Raw period-register wobble scale,
                                  // not calibrated to cents.
    uint8_t  vibrato_speed;      // phase increment per frame
    uint8_t  vibrato_delay;      // frames after trigger before vibrato ramps in
    uint8_t  gate_off_timer;     // frames after trigger to force silence; 0 = never
                                  // (voice stays held until MIDI note-off) --
                                  // AY's own release mechanism
};
