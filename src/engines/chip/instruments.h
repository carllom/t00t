#pragma once

#include "instrument.h"
#include "chip/sid_filter.h"   // SID_FILT_LP

// Hand-authored instruments (sid.md §6 P3), analogous to the groovebox's
// kit.h -- P4's .ins converter replaces hand-authoring with real GoatTracker
// data, but the VM needs *something* to prove it steps tables correctly
// against reSID and, more usefully here, against ears. One instrument per
// documented table feature rather than one "does everything" patch, so a
// wrong table shows up as one wrong instrument, not a wrong chord.

// --- ARP_LEAD: major-triad arpeggio, no pulse/filter sweep ----------------
// Wave-table rows have no duration field (unlike pulse/filter's SweepRow) --
// one row is exactly one frame, so holding a note longer means repeating
// its row, the standard tracker convention. A bare 4-row table (one row per
// note) cycles at 50/4 = 12.5 Hz, 20 ms/note -- far faster than a musical
// arpeggio and the likely cause of an early "rough/grainy" by-ear report.
// 6x repeats each note for 120 ms, 480 ms/cycle (~2.1 Hz) instead.
static const WaveRow ARP_LEAD_WAVE[] = {
    { 0x2, 0, 0  }, { 0x2, 0, 0  },   // root
    { 0x2, 0, 4  }, { 0x2, 0, 4  },   // major third
    { 0x2, 0, 7  }, { 0x2, 0, 7  },   // fifth
    { 0x2, 0, 12 }, { 0x2, 0, 12 },   // octave
};

// --- PWM_PLUCK: pulse-width sweep, timed release regardless of hold -------
// A flat delta straight into a delta=0 hold has a continuous *value* but a
// discontinuous *rate of change* -- reported as "jumps to another value"
// even though the pulse width itself never actually jumps (verified: the
// sweep lands exactly on 3600, and the hold row starts from exactly there).
// The ear hears the derivative kink, not a value discontinuity. Tapered
// over the last few rows instead of a hard stop, same 40-frame total.
static const WaveRow PWM_PLUCK_WAVE[] = { { 0x4, 0, 0 } };
static const SweepRow PWM_PLUCK_PULSE[] = {
    { 90, 32 },    // ramp ~2880 over 32 frames
    { 45, 4 },     // decelerate
    { 20, 2 },
    { 8, 2 },      // ~3116 total, easing into the hold instead of stopping cold
    { 0, 250 },    // hold open
};

// --- FILTER_PAD: LP cutoff sweep, slow attack, gentle vibrato -------------
static const WaveRow FILTER_PAD_WAVE[] = { { 0x6, 0, 0 } };   // SAW|PULSE
static const SweepRow FILTER_PAD_FILTER[] = {
    { 30, 60 },    // sweep open over ~1.2 s
    { 0, 250 },    // hold
};

// --- VIBRATO_LEAD: prominent delayed vibrato, no tables at all ------------
static const WaveRow VIBRATO_LEAD_WAVE[] = { { 0x2, 0, 0 } };

enum ChipInstrumentId : uint8_t {
    INS_ARP_LEAD = 0,
    INS_PWM_PLUCK,
    INS_FILTER_PAD,
    INS_VIBRATO_LEAD,
    INSTRUMENT_COUNT,
};

// Order must match ChipInstrumentId (plain array indexing, no designators --
// GCC's [index]= extension is non-portable and unnecessary here).
static const Instrument INSTRUMENTS[INSTRUMENT_COUNT] = {
    // INS_ARP_LEAD
    {
        (uint8_t)(1 << 4 | 6), (uint8_t)(10 << 4 | 5), 0,  0, 0, 0,  0,  0x2,
        { ARP_LEAD_WAVE, 8, 0 },      // wave table (4 notes x 6-frame hold), loops from row 0
        { nullptr, 0, 0 }, 0,          // no pulse sweep; pulse_init unused (pure SAW)
        false, 0, 0, 0, { nullptr, 0, 0 },
    },
    // INS_PWM_PLUCK -- gate_off_timer 45 (was 30): the sweep itself now
    // takes 40 frames (32+4+2+2, tapered), so firing release at 30 cut the
    // pluck's own signature effect off before it finished. 45 lets it settle
    // into the hold first.
    {
        (uint8_t)(0 << 4 | 4), (uint8_t)(4 << 4 | 3), 0,  0, 0, 0,  45,  0x4,
        { PWM_PLUCK_WAVE, 1, 0 },
        { PWM_PLUCK_PULSE, 5, 4 }, 200, // loop=4: hold forever; pulse_init 200,
                                        // not 0 -- pw=0 is degenerate for pure
                                        // pulse (top12>=0 is always true, so
                                        // it outputs a constant, not a tone,
                                        // until the first tick corrects it)
        false, 0, 0, 0, { nullptr, 0, 0 },
    },
    // INS_FILTER_PAD -- vibrato_speed 40 =~ 2.0 Hz, a gentle pad-scale wobble
    // (was 6 =~ 0.29 Hz / 3.4 s per cycle, an uncalibrated arbitrary pick --
    // see audio_engine.cpp's vibrato math comment).
    {
        (uint8_t)(9 << 4 | 9), (uint8_t)(12 << 4 | 10), 0,  15, 40, 20,  0,  0x6,
        { FILTER_PAD_WAVE, 1, 0 },
        { nullptr, 0, 0 }, 2048,        // no sweep, but waveform 0x6 = SAW|PULSE
                                        // does use pw for the AND-combination
                                        // (§4.2) -- pulse_init 2048 (50% duty)
                                        // instead of 0, or the PULSE branch
                                        // degenerates to a no-op and this
                                        // "combined" waveform is just SAW
        true, 200, 8, SID_FILT_LP,
        { FILTER_PAD_FILTER, 2, 1 },
    },
    // INS_VIBRATO_LEAD -- vibrato_speed 110 =~ 5.4 Hz, ordinary vocal/
    // instrumental vibrato range (was 10 =~ 0.49 Hz / 2.0 s per cycle,
    // reported as "quite slow, perhaps over a second per cycle" -- same
    // uncalibrated-constant class of miss as vibrato_depth's original >>8).
    {
        (uint8_t)(3 << 4 | 7), (uint8_t)(11 << 4 | 8), 0,  40, 110, 15,  0,  0x2,
        { VIBRATO_LEAD_WAVE, 1, 0 },
        { nullptr, 0, 0 }, 0,           // pure SAW, pulse_init unused
        false, 0, 0, 0, { nullptr, 0, 0 },
    },
};
