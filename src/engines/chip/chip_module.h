#pragma once

#include "engine.h"
#include "instrument.h"         // WaveRow, SweepRow, WAVE_HOLD
#include "chip/ay_envelope.h"   // AyModel
#include "patch/descriptor.h"
#include "patch/module_id.h"
#include "patch/multi_cooked.h"
#include "patch/part_slot.h"
#include <cstdint>

// The chip module on the patch platform (docs/platform.md): its descriptor
// table -- one instrument, SID or AY, with its frame tables -- its cooked
// patch, and the cook step between them.
//
// SID and AY instruments share one schema. A Patch's TYPE says which chip
// plays it; the tables serve both: the wave table's notes are an AY
// instrument's arpeggio (its waveform bits unused), the pulse table its
// software volume envelope, and the filter table is SID-only. Each chip's
// own settings are ignored by the other.

// One Part per MIDI channel by default: Part N plays on channel N.
static constexpr uint32_t CHIP_PART_COUNT = 16;

// Rows per frame table.
static constexpr uint32_t CHIP_TABLE_ROWS = 24;

// A wave row's parameter: the SID waveform bits (sid_osc.h's SidWave), the
// reserved sync and ring toggles, and HOLD -- keep the previous row's
// waveform and change only the note.
enum ChipWaveRowBits : uint8_t {
    CHIP_ROW_WAVE_MASK = 0x0F,
    CHIP_ROW_SYNC = 0x10,
    CHIP_ROW_RING = 0x20,
    CHIP_ROW_HOLD = 0x40,
};

// Parameter IDs (permanent; append only). The instrument-wide parameters
// come first, then each table's rows: a wave row is WAVE and NOTE, a sweep
// row (pulse, filter) DELTA and FRAMES.
enum ChipParamId : uint16_t {
    CHIP_P_TYPE,
    CHIP_P_ATTACK,
    CHIP_P_DECAY,
    CHIP_P_SUSTAIN,
    CHIP_P_RELEASE,
    CHIP_P_VIB_DEPTH,
    CHIP_P_VIB_SPEED,
    CHIP_P_VIB_DELAY,
    CHIP_P_GATE_OFF,
    CHIP_P_FIRST_WAVE,
    CHIP_P_PULSE_INIT,
    CHIP_P_FILTER,
    CHIP_P_CUTOFF,
    CHIP_P_RESONANCE,
    CHIP_P_AY_MODEL,
    CHIP_P_AY_VOLUME,
    CHIP_P_AY_LEVEL,
    CHIP_P_AY_ENV_PERIOD,
    CHIP_P_AY_ENV_SHAPE,
    CHIP_P_AY_MIXER,
    CHIP_P_AY_NOISE_PERIOD,
    CHIP_P_WAVE_LEN,
    CHIP_P_WAVE_LOOP,
    CHIP_P_PULSE_LEN,
    CHIP_P_PULSE_LOOP,
    CHIP_P_FILTER_LEN,
    CHIP_P_FILTER_LOOP,
    CHIP_P_WAVE_ROWS,
    CHIP_P_PULSE_ROWS = CHIP_P_WAVE_ROWS + 2 * CHIP_TABLE_ROWS,
    CHIP_P_FILTER_ROWS = CHIP_P_PULSE_ROWS + 2 * CHIP_TABLE_ROWS,
    CHIP_P_COUNT = CHIP_P_FILTER_ROWS + 2 * CHIP_TABLE_ROWS,
};

// The IDs of row `r`'s fields (0 = the first row).
constexpr uint16_t chip_wave_row_wave(uint32_t r) { return (uint16_t)(CHIP_P_WAVE_ROWS + 2 * r); }
constexpr uint16_t chip_wave_row_note(uint32_t r) { return (uint16_t)(CHIP_P_WAVE_ROWS + 2 * r + 1); }
constexpr uint16_t chip_pulse_row_delta(uint32_t r) { return (uint16_t)(CHIP_P_PULSE_ROWS + 2 * r); }
constexpr uint16_t chip_pulse_row_frames(uint32_t r) { return (uint16_t)(CHIP_P_PULSE_ROWS + 2 * r + 1); }
constexpr uint16_t chip_filter_row_delta(uint32_t r) { return (uint16_t)(CHIP_P_FILTER_ROWS + 2 * r); }
constexpr uint16_t chip_filter_row_frames(uint32_t r) { return (uint16_t)(CHIP_P_FILTER_ROWS + 2 * r + 1); }

// Value labels for the ENUM parameters, in value order; bit names for the
// BITS parameters, from bit 0.
inline constexpr const char *kChipTypeLabels[] = { "SID", "AY" };
inline constexpr const char *kChipWaveBits[] = { "TRI", "SAW", "PUL", "NOI" };
inline constexpr const char *kChipWaveRowBits[] = { "TRI", "SAW", "PUL", "NOI", "SYNC", "RING", "HOLD" };
inline constexpr const char *kChipFilterBits[] = { "LP", "BP", "HP" };
inline constexpr const char *kChipAyModelLabels[] = { "AY-3-8910", "YM2149" };
inline constexpr const char *kChipAyLevelLabels[] = { "VOLUME", "ENVELOPE" };
inline constexpr const char *kChipAyMixerBits[] = { "TONE", "NOISE" };

// A frame table as the engine reads it: rows past `len` are never read;
// `loop` is the row the table jumps to when it runs off the end, or holds
// the last row if `loop >= len`.
struct ChipWaveTable {
    WaveRow rows[CHIP_TABLE_ROWS];
    uint8_t len;
    uint8_t loop;
};
struct ChipSweepTable {
    SweepRow rows[CHIP_TABLE_ROWS];
    uint8_t len;
    uint8_t loop;
};

// The engine-native form of one Patch: the instrument the frame VM plays.
// Every field is fixed for a note when it starts.
struct ChipCookedPatch {
    VoiceType type;              // VT_SID or VT_AY

    // Both chips.
    uint16_t vibrato_depth;      // 0 = off; a raw register wobble scale
    uint8_t  vibrato_speed;      // phase increment per frame
    uint8_t  vibrato_delay;      // frames after trigger before vibrato starts
    uint8_t  gate_off_timer;     // frames after trigger to end the note; 0 = never

    // SID.
    uint8_t  ad, sr;             // ADSR nibbles
    uint8_t  first_wave;         // SidWave bits at trigger, before the wave table's first row
    uint16_t pulse_init;         // pulse width at trigger
    bool     uses_filter;        // routes through a filter bus
    uint8_t  filter_mode_mask;   // SID_FILT_LP|BP|HP
    uint16_t filter_cutoff_init;
    uint8_t  filter_resonance;

    // AY.
    AyModel  ay_model;
    uint8_t  ay_initial_volume;  // 0-15
    bool     ay_use_envelope;    // the hardware envelope sets the level, not the volume
    uint16_t ay_envelope_period;
    uint8_t  ay_envelope_shape;
    bool     ay_tone_on;
    bool     ay_noise_on;
    uint8_t  ay_noise_period;

    ChipWaveTable  wave;    // SID: waveform and note per frame; AY: note per frame
    ChipSweepTable pulse;   // SID: pulse width sweep; AY: volume sweep
    ChipSweepTable filter;  // SID: cutoff sweep
};

// Cooked-patch slots, one per Part, shared between Core 0 (publishes) and
// Core 1 (consumes once per buffer). Defined by the Input subsystem.
extern PartSlotT<ChipCookedPatch> chip_part_slots[CHIP_PART_COUNT];

// The cooked Multi (mix and FX) and its slot, likewise.
using ChipMultiCooked = MultiCookedT<CHIP_PART_COUNT>;
extern PartSlotT<ChipMultiCooked> chip_multi_slot;

#define CHIP_D(ID, NAME, GROUP, MIN, MAX, DEF, UNIT, QUICK) \
    { ID, NAME, GROUP, MIN, MAX, DEF, ParamUnit::UNIT, ParamCurve::LINEAR, ParamClass::NOTE_ON, QUICK, kNoDefaultCc }
#define CHIP_L(ID, NAME, GROUP, MAX, DEF, UNIT, LABELS)                                                    \
    { ID, NAME, GROUP, 0, MAX, DEF, ParamUnit::UNIT, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, \
      LABELS, sizeof LABELS / sizeof LABELS[0] }

// A row's descriptors. Unused rows default to a pure-arpeggio wave row
// (HOLD, no note offset) and a one-frame sweep row with no change.
#define CHIP_WAVE_ROW(N)                                                                                    \
    CHIP_L(chip_wave_row_wave(N - 1), "W" #N " WAVE", "WAVE/TONE TABLE", 127, CHIP_ROW_HOLD, BITS, kChipWaveRowBits), \
    CHIP_D(chip_wave_row_note(N - 1), "W" #N " NOTE", "WAVE/TONE TABLE", -24, 24, 0, NONE, false)
#define CHIP_PULSE_ROW(N)                                                                                   \
    CHIP_D(chip_pulse_row_delta(N - 1), "P" #N " DELTA", "PULSE/VOLUME TABLE", -4095, 4095, 0, NONE, false), \
    CHIP_D(chip_pulse_row_frames(N - 1), "P" #N " FRAMES", "PULSE/VOLUME TABLE", 0, 255, 1, NONE, false)
#define CHIP_FILTER_ROW(N)                                                                                  \
    CHIP_D(chip_filter_row_delta(N - 1), "F" #N " DELTA", "FILTER TABLE", -2047, 2047, 0, NONE, false),     \
    CHIP_D(chip_filter_row_frames(N - 1), "F" #N " FRAMES", "FILTER TABLE", 0, 255, 1, NONE, false)
#define CHIP_ROWS_24(ROW)                                                                                   \
    ROW(1), ROW(2), ROW(3), ROW(4), ROW(5), ROW(6), ROW(7), ROW(8), ROW(9), ROW(10), ROW(11), ROW(12),     \
    ROW(13), ROW(14), ROW(15), ROW(16), ROW(17), ROW(18), ROW(19), ROW(20), ROW(21), ROW(22), ROW(23), ROW(24)

struct ChipModule {
    static constexpr ModuleId kModuleId = ModuleId::CHIP;
    static constexpr uint8_t kSchemaVersion = 1;
    static constexpr uint32_t kPartCount = CHIP_PART_COUNT;
    using Cooked = ChipCookedPatch;

    // The default Multi plays factory bank 0, slot 0 on every Part.
    static constexpr uint8_t default_program(uint32_t) { return 0; }

    // Defaults are an INIT instrument: a sustained SID sawtooth, no tables.
    static constexpr ParamDescriptor kDescriptors[CHIP_P_COUNT] = {
        CHIP_L(CHIP_P_TYPE, "TYPE", "VOICE", 1, 0, ENUM, kChipTypeLabels),
        CHIP_D(CHIP_P_ATTACK, "ATTACK", "SID", 0, 15, 0, NONE, true),
        CHIP_D(CHIP_P_DECAY, "DECAY", "SID", 0, 15, 0, NONE, true),
        CHIP_D(CHIP_P_SUSTAIN, "SUSTAIN", "SID", 0, 15, 15, NONE, true),
        CHIP_D(CHIP_P_RELEASE, "RELEASE", "SID", 0, 15, 4, NONE, true),
        CHIP_D(CHIP_P_VIB_DEPTH, "VIB DEPTH", "VOICE", 0, 255, 0, NONE, true),
        CHIP_D(CHIP_P_VIB_SPEED, "VIB SPEED", "VOICE", 0, 255, 0, NONE, false),
        CHIP_D(CHIP_P_VIB_DELAY, "VIB DELAY", "VOICE", 0, 255, 0, NONE, false),
        CHIP_D(CHIP_P_GATE_OFF, "GATE OFF", "VOICE", 0, 255, 0, NONE, false),
        CHIP_L(CHIP_P_FIRST_WAVE, "FIRST WAVE", "SID", 15, 0x2, BITS, kChipWaveBits),
        CHIP_D(CHIP_P_PULSE_INIT, "PULSE INIT", "SID", 0, 4095, 2048, NONE, false),
        CHIP_L(CHIP_P_FILTER, "FILTER", "SID FILTER", 7, 0, BITS, kChipFilterBits),
        CHIP_D(CHIP_P_CUTOFF, "CUTOFF", "SID FILTER", 0, 2047, 1024, NONE, true),
        CHIP_D(CHIP_P_RESONANCE, "RESONANCE", "SID FILTER", 0, 15, 0, NONE, false),
        CHIP_L(CHIP_P_AY_MODEL, "MODEL", "AY", 1, 0, ENUM, kChipAyModelLabels),
        CHIP_D(CHIP_P_AY_VOLUME, "VOLUME", "AY", 0, 15, 12, NONE, false),
        CHIP_L(CHIP_P_AY_LEVEL, "LEVEL FROM", "AY", 1, 0, ENUM, kChipAyLevelLabels),
        CHIP_D(CHIP_P_AY_ENV_PERIOD, "ENV PERIOD", "AY", 0, 65535, 0, NONE, false),
        CHIP_D(CHIP_P_AY_ENV_SHAPE, "ENV SHAPE", "AY", 0, 15, 0, NONE, false),
        CHIP_L(CHIP_P_AY_MIXER, "MIXER", "AY", 3, 1, BITS, kChipAyMixerBits),
        CHIP_D(CHIP_P_AY_NOISE_PERIOD, "NOISE PERIOD", "AY", 0, 31, 0, NONE, false),
        CHIP_D(CHIP_P_WAVE_LEN, "WAVE LENGTH", "WAVE/TONE TABLE", 0, CHIP_TABLE_ROWS, 0, NONE, false),
        CHIP_D(CHIP_P_WAVE_LOOP, "WAVE LOOP", "WAVE/TONE TABLE", 0, CHIP_TABLE_ROWS, 0, NONE, false),
        CHIP_D(CHIP_P_PULSE_LEN, "PULSE LENGTH", "PULSE/VOLUME TABLE", 0, CHIP_TABLE_ROWS, 0, NONE, false),
        CHIP_D(CHIP_P_PULSE_LOOP, "PULSE LOOP", "PULSE/VOLUME TABLE", 0, CHIP_TABLE_ROWS, 0, NONE, false),
        CHIP_D(CHIP_P_FILTER_LEN, "FILTER LENGTH", "FILTER TABLE", 0, CHIP_TABLE_ROWS, 0, NONE, false),
        CHIP_D(CHIP_P_FILTER_LOOP, "FILTER LOOP", "FILTER TABLE", 0, CHIP_TABLE_ROWS, 0, NONE, false),
        CHIP_ROWS_24(CHIP_WAVE_ROW),
        CHIP_ROWS_24(CHIP_PULSE_ROW),
        CHIP_ROWS_24(CHIP_FILTER_ROW),
    };

    static WaveRow cook_wave_row(int32_t bits, int32_t note) {
        WaveRow r;
        r.waveform = (bits & CHIP_ROW_HOLD) ? WAVE_HOLD : (uint8_t)(bits & CHIP_ROW_WAVE_MASK);
        r.flags = (uint8_t)(((bits & CHIP_ROW_SYNC) ? WAVE_FLAG_SYNC : 0) | ((bits & CHIP_ROW_RING) ? WAVE_FLAG_RING : 0));
        r.note = (int8_t)note;
        return r;
    }

    static void cook_sweep(const int32_t *v, uint16_t len_id, uint16_t rows_id, ChipSweepTable &t) {
        t.len = (uint8_t)v[len_id];
        t.loop = (uint8_t)v[len_id + 1];
        for (uint32_t r = 0; r < CHIP_TABLE_ROWS; r++) {
            t.rows[r].delta = (int16_t)v[rows_id + 2 * r];
            t.rows[r].duration = (uint8_t)v[rows_id + 2 * r + 1];
        }
    }

    // Values -> the instrument the frame VM plays.
    static void cook(const int32_t *v, ChipCookedPatch &c) {
        c.type = v[CHIP_P_TYPE] ? VT_AY : VT_SID;
        c.vibrato_depth = (uint16_t)v[CHIP_P_VIB_DEPTH];
        c.vibrato_speed = (uint8_t)v[CHIP_P_VIB_SPEED];
        c.vibrato_delay = (uint8_t)v[CHIP_P_VIB_DELAY];
        c.gate_off_timer = (uint8_t)v[CHIP_P_GATE_OFF];

        c.ad = (uint8_t)(v[CHIP_P_ATTACK] << 4 | v[CHIP_P_DECAY]);
        c.sr = (uint8_t)(v[CHIP_P_SUSTAIN] << 4 | v[CHIP_P_RELEASE]);
        c.first_wave = (uint8_t)v[CHIP_P_FIRST_WAVE];
        c.pulse_init = (uint16_t)v[CHIP_P_PULSE_INIT];
        // FILTER's bits are SID_FILT_LP|BP|HP; none set routes the voice dry.
        c.filter_mode_mask = (uint8_t)v[CHIP_P_FILTER];
        c.uses_filter = c.filter_mode_mask != 0;
        c.filter_cutoff_init = (uint16_t)v[CHIP_P_CUTOFF];
        c.filter_resonance = (uint8_t)v[CHIP_P_RESONANCE];

        c.ay_model = v[CHIP_P_AY_MODEL] ? AY_MODEL_YM2149 : AY_MODEL_AY8910;
        c.ay_initial_volume = (uint8_t)v[CHIP_P_AY_VOLUME];
        c.ay_use_envelope = v[CHIP_P_AY_LEVEL] != 0;
        c.ay_envelope_period = (uint16_t)v[CHIP_P_AY_ENV_PERIOD];
        c.ay_envelope_shape = (uint8_t)v[CHIP_P_AY_ENV_SHAPE];
        c.ay_tone_on = (v[CHIP_P_AY_MIXER] & 1) != 0;
        c.ay_noise_on = (v[CHIP_P_AY_MIXER] & 2) != 0;
        c.ay_noise_period = (uint8_t)v[CHIP_P_AY_NOISE_PERIOD];

        c.wave.len = (uint8_t)v[CHIP_P_WAVE_LEN];
        c.wave.loop = (uint8_t)v[CHIP_P_WAVE_LOOP];
        for (uint32_t r = 0; r < CHIP_TABLE_ROWS; r++) {
            c.wave.rows[r] = cook_wave_row(v[chip_wave_row_wave(r)], v[chip_wave_row_note(r)]);
        }
        cook_sweep(v, CHIP_P_PULSE_LEN, CHIP_P_PULSE_ROWS, c.pulse);
        cook_sweep(v, CHIP_P_FILTER_LEN, CHIP_P_FILTER_ROWS, c.filter);
    }
};

#undef CHIP_D
#undef CHIP_L
#undef CHIP_WAVE_ROW
#undef CHIP_PULSE_ROW
#undef CHIP_FILTER_ROW
#undef CHIP_ROWS_24
