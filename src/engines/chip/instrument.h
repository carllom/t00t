#pragma once

#include <cstdint>

// The frame-rate instrument table VM (module_chip.md §6): ADSR + vibrato +
// three per-frame tables. This is the mechanism; engines/chip/instruments.h
// holds hand-authored data.
//
// Format notes not fully pinned down by module_chip.md itself, decided here:
//
// - WaveRow.note is a *relative* semitone offset from the played note
//   (arpeggios), not an absolute note -- module_chip.md says "note abs/rel" without
//   picking one, and relative is what every table-model editor's arpeggio
//   row actually is. Absolute-note rows are not built.
// - WaveRow.flags reserves 2 bits for sync/ring toggles per §6's table, but
//   nothing sets them yet: the §4.4 per-voice sub-oscillator (mod_acc/
//   mod_inc/mod_mode) exists only in the rig and the CHIP_STRICT harness's
//   adjacency topology, not in the real engine. Building a half of "sync
//   toggle" without the sub-oscillator underneath it would be a no-op
//   that looks implemented.
// - mod_inc sweeps (§4.4) are the same gap -- there is no mod_inc to
//   sweep yet.
// - Table loop semantics: `loop` is the row index jumped to when the table
//   runs off the end; `loop >= len` means "hold the last row forever"
//   instead of wrapping (GoatTracker's convention for a non-looping table).
// - Pulse/filter tables share one row shape (SweepRow: a per-frame delta
//   held for `duration` frames before advancing) since both are PWM/cutoff
//   ramps with identical stepping logic, just different targets.

// Waveform-or-command byte for a wave-table row. 0x00-0x0f are SidWave bits
// (sid_osc.h) directly; 0xff means "hold the previous row's waveform,
// change only the note" -- a pure-arpeggio row.
static constexpr uint8_t WAVE_HOLD = 0xff;

enum WaveRowFlags : uint8_t {
    WAVE_FLAG_SYNC = 0x01,   // reserved -- see header comment, not wired yet
    WAVE_FLAG_RING = 0x02,   // reserved -- see header comment, not wired yet
};

struct WaveRow {
    uint8_t waveform;   // SidWave bits, or WAVE_HOLD
    uint8_t flags;      // WaveRowFlags
    int8_t  note;       // semitone offset from the played note
};

struct SweepRow {
    int16_t delta;      // added to the current value once per frame
    uint8_t duration;   // frames to hold this delta before the next row
};

struct WaveTable  { const WaveRow  *rows; uint8_t len; uint8_t loop; };
struct SweepTable { const SweepRow *rows; uint8_t len; uint8_t loop; };

struct Instrument {
    uint8_t ad, sr;              // ADSR nibbles (§4.3)
    uint8_t hard_restart;        // format completeness for .ins import only --
                                  // t00t always hard-restarts instantly (§4.3),
                                  // so this is never read.
    uint8_t vibrato_depth;       // 0 = off. Raw freq-register wobble scale, not
                                  // yet calibrated to cents/semitones -- by-ear
                                  // tuning item.
    uint8_t vibrato_speed;       // phase increment per frame
    uint8_t vibrato_delay;       // frames after trigger before vibrato ramps in
    uint8_t gate_off_timer;      // frames after trigger to force gate_off(); 0 = never
                                  // (voice stays held until MIDI note-off, or forever
                                  // if the frame VM has no other way to end it)
    uint8_t first_wave;          // SidWave bits applied immediately at trigger,
                                  // before the wave table's own row 0 takes effect
                                  // at the first frame tick

    WaveTable  wave;
    SweepTable pulse;
    uint16_t   pulse_init;        // pw at trigger, before the pulse table's first
                                    // row takes effect. 0 is a real degenerate value
                                    // for a pure pulse waveform, not a safe default:
                                    // sid_pulse()'s top12>=pw is true for every
                                    // top12 when pw=0, so the oscillator outputs a
                                    // constant (non-oscillating) level -- a DC-like
                                    // step, not audio -- until the first frame tick
                                    // corrects it. Pick a real starting pulse width.

    bool     uses_filter;        // request a bus at trigger time (module_chip.md §5.2 bind_filter)
    uint16_t filter_cutoff_init;
    uint8_t  filter_resonance;
    uint8_t  filter_mode_mask;   // SID_FILT_LP|BP|HP (sid_filter.h), summed
    SweepTable filter;
};
