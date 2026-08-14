#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "audio_common.h"
#include "blob_format.h"
#include "mixer.h"
#include "pan.h"

// Core 0 tracker player: order-list/pattern walk, note triggering, the
// TickBlock/TickRing shapes from the design doc, per-tick effect state
// machines (arpeggio, porta up/down, tone porta, vibrato, volume slide, set
// volume/panning, position jump, pattern break, set speed/tempo, 9xx sample
// offset, ping-pong loop direction, and the bounded Exy sub-commands -- fine
// porta E1x/E2x, fine volslide EAx/EBx, note cut ECx
// (tracker_apply_tick_note_cut()), note delay EDx (player_produce_tick()'s
// row_boundary dispatch), pattern delay EEx (same function's rollover), and
// retrigger E9x/Rxy (tracker_apply_tick_retrigger())), and per-instrument
// state (envelopes, fadeout, autovibrato) resolved once per tick regardless
// of whether a pattern effect is active. Pure integer/double math over a
// `SongHeader*` blob, no pico-sdk: this header is included by both
// tools/host_render/render_xm_device.cpp (the reference-diff harness) and
// the real Core 0 engine (player_task.cpp). `player_produce_tick()` is a
// pure function of `PlayerState` plus the blob -- identical behaviour on
// host or device is the whole point (module_tracker.md: "Any divergence between the
// host and device render paths defeats the purpose").
//
// Still no-ops: glissando control (E3x -- tried, reverted: two reasonable
// snap-to-semitone implementations both diverged from openmpt123 within a
// couple of ticks, which makes this genuinely diff-driven quirk work, not
// the bounded/mechanical case the rest of this slice is), tremolo, tremor,
// global volume, effect-column panning slide, and the four named FT2 quirks
// (E60 pattern loop, envelope-on-note-off, portamento-with-instrument-change,
// arpeggio wraparound) -- deliberately deferred, module_tracker.md's "long
// tail of FT2 quirks". Key-off (note 97) does not cut a voice directly: it
// only marks the channel key_off, which the envelope/fadeout machinery
// (tracker_resolve_envelope_volpan()) consumes every tick from then on -- an
// instrument with an enabled volume envelope releases through it (plus
// fadeout, once the envelope itself isn't holding sustain); one with no
// envelope at all cuts (almost) instantly, matching openmpt123 (see
// tracker_resolve_envelope_volpan()'s header comment).
//
// Requires osc_init_sine() to have been called first (pan_gains_q15's
// quadrature source), same precondition as mixer.h's consumers.

// Ring depth: 2 slots is sufficient given 20ms of tick slack per
// module_tracker.md's own reasoning; a host harness driving this
// synchronously doesn't need lookahead at all, and the real cross-core case
// (TickRing below) inherits this constant as-is.
static constexpr uint32_t TICK_RING_DEPTH = 2;

// Fixed at 32 regardless of a given song's num_channels (2-32): TickBlock is
// sized for the format's ceiling, matching engine.h's MAX_VOICES. Kept as a
// local constant rather than including engine.h, which drags in
// engine_base.h -> pico-sdk headers this host-buildable file must not touch.
static constexpr uint32_t TRACKER_MAX_CHANNELS = 32;

enum ChannelTickFlags : uint8_t {
    TICK_NOTE_ON  = 0x01,  // (re)triggered this tick -- reset position, latch inc
    TICK_KEY_OFF  = 0x02,  // note-off (XM note 97) -- pre-envelope: volume target cut to 0
    TICK_NOTE_CUT = 0x04,  // ECx (Note Cut) fired this tick -- vol64 was just zeroed
};

// One channel's state as of this tick. Restated every tick, not just on
// change -- module_tracker.md's render loop pseudocode re-applies whatever the
// latest TickBlock says unconditionally ("apply_tick(tick): latch
// inc/targets/triggers"), so the consumer never needs its own "did this
// change" logic. `trigger` is what lets it tell a restated-but-unchanged
// note apart from a genuine retrigger.
struct ChannelTick {
    uint32_t inc;                 // Q8.24, 0 = channel silent
    int32_t tgt_volL, tgt_volR;   // Q15, post-pan
    uint32_t sample_id;           // global sample index (SongHeader's sample table)
    uint32_t start_pos;           // Q18.14; 0 unless this tick's trigger carried a 9xx sample offset
    uint8_t trigger;              // generation counter, bumped on note-on
    uint8_t flags;                // ChannelTickFlags
};

struct TickBlock {
    uint32_t samples_per_tick;
    ChannelTick ch[TRACKER_MAX_CHANNELS];
};

// Single-producer/single-consumer ring, genuinely cross-core safe: Core 0 is
// the sole writer of `head`, Core 1 the sole writer of `tail`. `push()`/
// `pop()` release-store their own index; `full()`/`empty()` acquire-load the
// *other* index before touching a slot, so the producer's slot write
// happens-before the consumer sees `head` advance, and the consumer's slot
// read happens-before the producer reuses that slot for a new write.
// std::atomic (rather than hand-rolled ARM barriers, cf. engine_base.h's
// ParamExchangeT) because this file has to stay host-buildable --
// tools/host_render links it with the host compiler, no pico-sdk headers
// allowed. Used single-threaded by the host harness; genuinely cross-core
// between Core 0's player task and Core 1's mixer in the real engine.
struct TickRing {
    TickBlock slots[TICK_RING_DEPTH];
    std::atomic<uint32_t> head{0};  // next slot to write -- Core 0 only
    std::atomic<uint32_t> tail{0};  // next slot to read -- Core 1 only

    bool full() const {
        return head.load(std::memory_order_relaxed) - tail.load(std::memory_order_acquire) >= TICK_RING_DEPTH;
    }
    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_relaxed);
    }
    TickBlock &write_slot() { return slots[head.load(std::memory_order_relaxed) % TICK_RING_DEPTH]; }
    void push() { head.fetch_add(1, std::memory_order_release); }
    TickBlock &read_slot() { return slots[tail.load(std::memory_order_relaxed) % TICK_RING_DEPTH]; }
    void pop() { tail.fetch_add(1, std::memory_order_release); }
};

// Per-channel memory carried between ticks: the currently-sounding inc/
// volume (restated into every produced ChannelTick), the "current
// instrument" FT2 remembers across notes that omit the instrument column,
// and the per-tick effect state machines -- pitch (`period`, the
// portamento/vibrato/arpeggio source of truth), volume, and each effect
// family's own memory slot (an XM row with a zero effect param reuses the
// last nonzero one, per-command -- porta up/down and tone porta each keep a
// separate memory, matching FT2).
struct PlayerChannelState {
    uint32_t inc = 0;
    int32_t volL = 0, volR = 0;
    uint32_t sample_id = 0;
    uint8_t instrument = 0;  // 1-based; 0 = none yet
    uint8_t trigger = 0;

    // Volume/pan source of truth -- volL/volR above are derived from these
    // (plus envelopes/fadeout) every tick by
    // tracker_resolve_envelope_volpan(). pan_xm stays in XM's 0..255
    // domain, not Q15, because the panning envelope's asymmetric formula
    // (tracker_resolve_envelope_volpan()) needs |pan - 128| in that domain;
    // conversion to Q15 happens once, at the end, via tracker_xm_pan_to_q15().
    uint32_t vol64 = 0;    // 0..64, XM convention
    uint32_t pan_xm = 128;  // 0..255, XM convention; 128 = center

    // Pitch source of truth. `period` is in the song's native period units
    // (linear or Amiga, module_tracker.md/periods.py convention) -- portamento and
    // tone portamento move it directly; arpeggio and vibrato compute a
    // *transient* offset from it each tick without writing back (they must
    // not leave the channel detuned once the effect stops). `base_note` /
    // `finetune` are the last triggered note's tuning, needed to recompute a
    // period for arpeggio's semitone offsets and a tone-porta target.
    double period = 0.0;
    double base_note = 0.0;
    double finetune = 0.0;
    double tone_porta_target = 0.0;

    uint8_t porta_up_memory = 0;
    uint8_t porta_down_memory = 0;
    uint8_t tone_porta_memory = 0;
    uint8_t volslide_memory = 0;
    // 9xx sample-offset memory. Unlike porta/tone-porta/volslide's
    // memory (substituted whenever the effect is restated, param or not),
    // this one is only ever written on a row that both carries 9xx *and*
    // actually triggers a note (tracker_trigger_note()'s own comment) --
    // verified against openmpt123: "the effect memory of the Offset command
    // is only updated when the command is placed next to a note."
    uint8_t sample_offset_memory = 0;
    uint8_t vibrato_speed = 0;
    uint8_t vibrato_depth = 0;
    uint8_t vibrato_pos = 0;  // 0..255, advances by vibrato_speed each active tick

    // Remaining Exy sub-commands. Fine porta/volslide (E1x/E2x/EAx/EBx)
    // apply once, at tick 0 only -- own memory slots, separate from the
    // continuous 1xx/2xx/Axy commands' (FT2 does not share them).
    uint8_t fine_porta_up_memory = 0;
    uint8_t fine_porta_down_memory = 0;
    uint8_t fine_volslide_up_memory = 0;
    uint8_t fine_volslide_down_memory = 0;
    // EDx: tick within the *current* row this channel's trigger is deferred
    // to; 0xFF = no delay pending. Set at the row's genuine tick 0 (instead
    // of processing the row immediately) and consumed the tick it matches --
    // see player_produce_tick()'s row_boundary dispatch.
    uint8_t note_delay_tick = 0xFF;

    // Resolved once at tick 0 from this row's event (memory already
    // substituted for a zero param), consumed by every later tick of the
    // row. An XM continuous effect must be restated every row it runs on --
    // an empty effect column here correctly clears this to NONE, stopping
    // whatever was running.
    Effect active_effect = Effect::NONE;
    uint8_t active_param = 0;

    // The volume column is a logically independent second effect slot
    // -- XM allows a volume-column command and an effect-column command on
    // the same row -- so its continuous commands (volslide, panslide,
    // vol-column vibrato/tone-porta) get their own active-effect slot and
    // per-command memory, mirroring active_effect/active_param above.
    VolEffect active_vol_effect = VolEffect::NONE;
    uint8_t active_vol_param = 0;
    uint8_t vol_volslide_memory = 0;
    uint8_t vol_panslide_memory = 0;

    // Instrument envelopes, key-off/fadeout, autovibrato. Reset on
    // every real trigger (tracker_trigger_note()); key_off persists across
    // ticks/rows until the next trigger, driving both the envelopes'
    // sustain-hold and fadeout's decay.
    bool key_off = false;
    uint32_t vol_env_pos = 0;
    uint32_t pan_env_pos = 0;
    double fadeout_vol = 32768.0;
    uint32_t autovib_pos = 0;
    double autovib_amp = 0.0;
    bool autovib_sweeping = false;
};

struct PlayerState {
    uint32_t order_idx = 0;
    uint32_t row = 0;
    uint32_t tick_in_row = 0;
    uint32_t speed = 6;              // ticks per row
    uint32_t samples_per_tick = 0;
    PlayerChannelState ch[TRACKER_MAX_CHANNELS];

    // Row-level pending transport effects (Bxx/Dxx): latched by whichever
    // channel's tick-0 event sets them (last channel index wins, matching
    // FT2's left-to-right per-row evaluation), consumed once at the row's
    // last tick where the normal row/order advance would otherwise happen.
    // Both can be pending at once -- "B and D on the same row" jumps to B's
    // order at D's row instead of row 0.
    bool jump_pending = false;
    uint32_t jump_target_order = 0;
    bool break_pending = false;
    uint32_t break_row = 0;

    // EEx (pattern delay): extra full-speed passes still owed on the
    // current row, and whether *this* call's tick_in_row == 0 is one of
    // those held repeats rather than a genuine new row -- see
    // player_produce_tick()'s row_boundary computation and rollover logic.
    uint32_t pattern_delay_remaining = 0;
    bool pattern_delay_holding = false;
};

// module_tracker.md "Fxx tempo changes ... samples_per_tick": 44100 * 2.5 / bpm,
// rounded to nearest rather than truncated so tick length doesn't
// systematically drift short over a long render.
inline uint32_t tracker_samples_per_tick(uint32_t bpm) {
    return (SAMPLE_RATE * 5u + bpm) / (2u * bpm);
}

inline void player_init(PlayerState &st, const SongHeader *song) {
    st = PlayerState{};
    st.speed = song->default_tempo;
    st.samples_per_tick = tracker_samples_per_tick(song->default_bpm);
}

// --- Blob accessors: every offset in the blob is byte-relative to `song`
// (blob_format.py: "never pointers -- meant to be memcpy'd into SRAM and
// read in place"), so these are all one add + reinterpret_cast. ---
inline const uint8_t *tracker_blob_base(const SongHeader *song) {
    return reinterpret_cast<const uint8_t *>(song);
}
inline const uint8_t *tracker_order_table(const SongHeader *song) {
    return tracker_blob_base(song) + song->order_table_offset;
}
inline const PatternHeader *tracker_pattern_table(const SongHeader *song) {
    return reinterpret_cast<const PatternHeader *>(tracker_blob_base(song) + song->pattern_table_offset);
}
inline const InstrumentHeader *tracker_instrument_table(const SongHeader *song) {
    return reinterpret_cast<const InstrumentHeader *>(tracker_blob_base(song) + song->instrument_table_offset);
}
inline const SampleHeader *tracker_sample_table(const SongHeader *song) {
    return reinterpret_cast<const SampleHeader *>(tracker_blob_base(song) + song->sample_table_offset);
}
inline const Event *tracker_pattern_events(const SongHeader *song, const PatternHeader &pat) {
    return reinterpret_cast<const Event *>(tracker_blob_base(song) + pat.event_offset);
}
inline const Event &tracker_event_at(const SongHeader *song, const PatternHeader &pat,
                                      uint32_t row, uint32_t ch, uint32_t num_channels) {
    return tracker_pattern_events(song, pat)[row * num_channels + ch];
}

// XM's 0..255 panning byte -> the engine's signed Q15 pan (-32768 full left,
// 32767 full right): byte-replicate to 16 bits then re-center, the standard
// N-bit -> 2N-bit expansion that hits both endpoints exactly.
inline int16_t tracker_xm_pan_to_q15(uint32_t xm_pan) {
    uint32_t p16 = (xm_pan << 8) | (xm_pan & 0xFF);
    return (int16_t)(int32_t)(p16 - 32768u);
}

// --- Period/frequency math: a runtime C++ port of
// tools/xm2t00t/periods.py's linear/Amiga formulas. periods.py's own table
// (SampleHeader.note_increments_offset) only covers the no-effects-active
// case -- computed once, offline, per (sample, note). Portamento, tone
// portamento, vibrato and arpeggio modulate pitch continuously tick-by-tick,
// which needs the formula itself on-device, not just its precomputed table.
// Deliberately the same double-precision formulas as the host converter
// (not a fixed-point or table-based reimplementation) so a channel with no
// active pitch effect still lands on exactly the note_increments table's
// Q8.24 value -- see tracker_tick_period() and tracker_process_effects_tick0(),
// which only ever call into this math when an effect is actually modulating
// pitch this tick. 32 channels x a few effects x ~50Hz is nowhere near enough double
// math to dent Core 0's budget (module_tracker.md: "three million cycles per 20ms
// tick" against "~4000 cycles of work").
static constexpr double TRACKER_XM_BASE_FREQ_HZ = 8363.0;
static constexpr double TRACKER_LINEAR_BASE_PERIOD = 10.0 * 12.0 * 16.0;  // 1920
static constexpr double TRACKER_LINEAR_FREQ_PERIOD = 6.0 * 12.0 * 16.0;   // 1152
static constexpr double TRACKER_LINEAR_OCTAVE_PERIODS = 12.0 * 16.0;      // 192
static constexpr int TRACKER_AMIGA_OCTAVE_BASE_NOTE = 36;                 // XM note of the table's C entry
static constexpr double TRACKER_AMIGA_PERIOD_TABLE[12] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
};
static constexpr double TRACKER_AMIGA_NTSC_CLOCK = 7159090.5;

// Floor division/modulo (Python's divmod semantics), needed because the
// Amiga table lookup must floor toward -infinity for a note below the
// table's anchor (relative_note can be negative), unlike C++'s truncating
// `/`/`%`.
inline void tracker_floordivmod(int a, int b, int &q, int &r) {
    q = a / b;
    r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        q -= 1;
        r += b;
    }
}

inline double tracker_amiga_period_at_note(int note_int) {
    int rel = note_int - TRACKER_AMIGA_OCTAVE_BASE_NOTE;
    int octave, idx;
    tracker_floordivmod(rel, 12, octave, idx);
    double base = TRACKER_AMIGA_PERIOD_TABLE[idx];
    return octave >= 0 ? base / (double)(1u << octave) : base * (double)(1u << (-octave));
}

// note: 0-based semitone index from C-0 (== sample.relative_note + played
// note), may carry a fractional part (arpeggio's semitone offsets are
// integral, but this stays double for a uniform signature). finetune:
// -128..127. linear: SongHeader::freq_table != 0.
inline double tracker_note_to_period(double note, double finetune, bool linear) {
    if (linear) {
        return TRACKER_LINEAR_BASE_PERIOD - note * 16.0 - finetune / 8.0;
    }
    double effective_note = note + finetune / 128.0;
    double n0 = std::floor(effective_note);
    double frac = effective_note - n0;
    double p0 = tracker_amiga_period_at_note((int)n0);
    double p1 = tracker_amiga_period_at_note((int)n0 + 1);
    return p0 + (p1 - p0) * frac;
}

inline double tracker_period_to_freq(double period, bool linear) {
    if (linear) {
        return TRACKER_XM_BASE_FREQ_HZ * std::pow(2.0, (TRACKER_LINEAR_FREQ_PERIOD - period) / TRACKER_LINEAR_OCTAVE_PERIODS);
    }
    if (period < 1.0) period = 1.0;
    return TRACKER_AMIGA_NTSC_CLOCK / (2.0 * period);
}

// module_tracker.md "Fixed-Point Formats": inc = f_note / f_mix, Q8.24, rounded to
// nearest and clamped -- identical convention to periods.py's
// q8_24_increment(), except the floor is 1 rather than 0. This function is
// only ever called for a channel a pitch effect is actively driving (a
// plain trigger latches straight from the precomputed note_increments
// table, untouched by this floor), and `inc == 0` is this format's "channel
// silent" sentinel (ChannelTick's own comment) -- an unbounded portamento
// pushing the period far enough to round-to-zero must not accidentally cut
// a voice that's still very much TICK_NOTE_ON, which otherwise divides by
// zero in mixer.h's samples_to_loop_end() (mix_voice() only ever checks
// v->active, never v->inc). The floor is 2048, not 1: mixer.h's
// tracker_latch_inc() right-shifts this Q8.24 value by 10 bits to reach
// TrackerVoice's Q18.14 increment, so anything below 1024 here still
// collapses to a latched 0 and hits the same division. 2048 keeps a margin
// above that shift's own floor.
inline uint32_t tracker_period_to_inc(double period, bool linear) {
    double freq = tracker_period_to_freq(period, linear);
    double inc = freq / (double)SAMPLE_RATE * (double)(1u << 24);
    if (inc < 2048.0) inc = 2048.0;
    if (inc > 4294967295.0) inc = 4294967295.0;
    return (uint32_t)(inc + 0.5);
}

// Sanity clamp on the persistent pitch state -- not a claimed-exact FT2
// limit (that level of precision belongs to the deep quirk tail, not this
// clamp), just a floor/ceiling so a long chain of unclamped portamento can't
// walk `period` somewhere tracker_period_to_freq() turns pathological.
inline void tracker_clamp_period(double &period) {
    if (period < 1.0) period = 1.0;
    if (period > 65535.0) period = 65535.0;
}

static constexpr uint8_t TRACKER_VIBRATO_SINE[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224,
    235, 244, 250, 253, 255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120, 97, 74, 49, 24,
};

// Instrument envelopes / key-off / volume column / autovibrato.
//
// Envelope flags (matches blob_format.py's _envelope_flags(): "bit0 enabled,
// bit1 sustain, bit2 loop").
enum : uint32_t {
    TRACKER_ENV_ENABLED = 0x01,
    TRACKER_ENV_SUSTAIN = 0x02,
    TRACKER_ENV_LOOP    = 0x04,
};

// One tick of one envelope (volume and panning share this shape): returns
// the envelope's value (0..64) at `pos` (the channel's running envelope
// tick, owned by the caller), then advances `pos` for the next call.
// Points are absolute-tick/value pairs (XM convention). Behaviourally
// follows FT2's envelope state machine -- sustain holds position exactly at
// the sustain point while the note is still held; looping wraps position
// back to loop_start once it reaches loop_end -- but recomputes the value
// fresh from the point table each tick via direct interpolation rather than
// FT2's own incremental Q8.8 delta-accumulation (which this engine has no
// need to replicate bit-for-bit: it exists in the original to avoid a
// division on 1990s hardware, not for behavioural reasons). Equivalent for
// well-formed envelopes, which is the overwhelming majority; loop-end/
// sustain-point coincidence edge cases are exactly the kind of "classic FT2
// divergence point" module_tracker.md defers to the quirk tail.
inline double tracker_envelope_tick(const EnvelopePoint *points, uint32_t count,
                                     uint32_t flags, uint32_t sustain_idx,
                                     uint32_t loop_start_idx, uint32_t loop_end_idx,
                                     uint32_t &pos, bool key_off) {
    if (count == 0) return 64.0;

    uint32_t last_tick = points[count - 1].tick;
    double value;
    if (count == 1 || pos <= points[0].tick) {
        value = (double)points[0].value;
    } else if (pos >= last_tick) {
        value = (double)points[count - 1].value;
    } else {
        value = (double)points[count - 1].value;  // unreachable fallback
        for (uint32_t i = 0; i + 1 < count; i++) {
            if (pos >= points[i].tick && pos <= points[i + 1].tick) {
                uint32_t t0 = points[i].tick, t1 = points[i + 1].tick;
                double v0 = (double)points[i].value, v1 = (double)points[i + 1].value;
                value = (t1 == t0) ? v0 : v0 + (v1 - v0) * (double)(pos - t0) / (double)(t1 - t0);
                break;
            }
        }
    }

    bool sustain_hold = (flags & TRACKER_ENV_SUSTAIN) && !key_off &&
                         sustain_idx < count && pos == points[sustain_idx].tick;
    if (!sustain_hold) {
        if (pos < last_tick) pos++;
        if ((flags & TRACKER_ENV_LOOP) && loop_end_idx < count && pos >= points[loop_end_idx].tick) {
            pos = points[loop_start_idx < count ? loop_start_idx : 0].tick;
        }
    }
    return value;
}

// Fadeout only ever moves after key-off, and never resets except on a fresh
// trigger (tracker_trigger_note()). Only called (tracker_resolve_envelope_
// volpan()) when the instrument's volume envelope is enabled -- fadeout is
// a *release* mechanism for an envelope-driven instrument, not a substitute
// for one. Verified against openmpt123: an instrument with no envelope at
// all cuts (almost) instantly at key-off regardless of its fadeout field,
// rather than fading over 32768/fadeout ticks -- see tracker_resolve_envelope_
// volpan()'s no-envelope branch for that case.
inline void tracker_fadeout_tick(const InstrumentHeader &inst, PlayerChannelState &pcs) {
    if (pcs.key_off) {
        pcs.fadeout_vol -= (double)inst.volume_fadeout;
        if (pcs.fadeout_vol < 0.0) pcs.fadeout_vol = 0.0;
    }
}

// 256-entry sine table for autovibrato's default waveform, amplitude ~64 to
// match FT2's own autoVibSineTab convention (ch->autoVibPos is a full byte,
// 0..255, unlike the effect-column vibrato's 32-entry quarter-wave table).
static constexpr int8_t TRACKER_AUTOVIB_SINE[256] = {
    0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 16, 17, 19, 20, 22, 23,
    24, 26, 27, 29, 30, 32, 33, 34, 36, 37, 38, 39, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 56, 57, 58, 59,
    59, 60, 60, 61, 61, 62, 62, 62, 63, 63, 63, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 63, 63, 63, 62, 62, 62, 61, 61, 60, 60,
    59, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46,
    45, 44, 43, 42, 41, 39, 38, 37, 36, 34, 33, 32, 30, 29, 27, 26,
    24, 23, 22, 20, 19, 17, 16, 14, 12, 11, 9, 8, 6, 5, 3, 2,
    0, -2, -3, -5, -6, -8, -9, -11, -12, -14, -16, -17, -19, -20, -22, -23,
    -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44,
    -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59,
    -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64,
    -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60,
    -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46,
    -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26,
    -24, -23, -22, -20, -19, -17, -16, -14, -12, -11, -9, -8, -6, -5, -3, -2,
};

// Standard XM volume-column tone-portamento coarse rate table (vol-column
// param 0..15 -> glide speed in this engine's period units/tick, same units
// as the effect-column 3xx param). Spot-checked against openmpt123
// (param 8 -> rate 128): a clean match. Not exhaustively verified against
// every one of the 16 entries -- a rare command (far less common in real
// modules than the effect-column 3xx it's an alternate spelling of), and
// once the rate is "fast enough" to reach the target note within a couple
// of ticks the render is identical for any such rate for the rest of the
// row, which makes distinguishing nearby candidate values empirically
// unreliable for anything but the smallest params. Indices 10..15 are
// effectively unused by real modules (the format only defines 0..9) but
// filled in for safety rather than left to read garbage. Long-tail
// precision here belongs to the deep quirk tail, same as the manual
// vibrato effect's own "not chased to bit-exactness" caveat.
static constexpr uint8_t TRACKER_VOLCOL_TONEPORTA_RATE[16] = {
    0, 1, 4, 8, 16, 32, 64, 96, 128, 255, 255, 255, 255, 255, 255, 255,
};

inline double tracker_volcol_toneporta_rate(uint8_t vol_param) {
    return (double)TRACKER_VOLCOL_TONEPORTA_RATE[vol_param & 0x0F];
}

// Autovibrato: per-instrument sweep/depth/rate/waveform pitch LFO, applied
// every tick on top of whatever the pattern effects computed -- FT2's
// updateVolPanAutoVib() auto-vibrato section, ported to this engine's
// double-precision period math (tracker_note_to_period()'s own convention,
// same reasoning as that function's header comment: nowhere near Core 0's
// budget). Sweep ramps amplitude from 0 to depth over `sweep` ticks (or
// straight to depth if sweep == 0), and freezes -- does not reset -- once
// the note is released. The resulting swing is intentionally small (~4
// period units at max depth): FT2's own internal period units are 4x finer
// than this engine's linear period scale (the same *4 factor as
// tracker_tick_period()'s porta comment), and autovibrato is meant to be a
// subtle chorus-like wobble, not a bend.
inline double tracker_autovibrato_delta(const InstrumentHeader &inst, PlayerChannelState &pcs) {
    if (inst.vibrato_depth == 0) return 0.0;

    double depth = (double)inst.vibrato_depth;
    if (inst.vibrato_sweep > 0 && pcs.autovib_sweeping) {
        pcs.autovib_amp += depth / (double)inst.vibrato_sweep;
        if (pcs.autovib_amp >= depth) {
            pcs.autovib_amp = depth;
            pcs.autovib_sweeping = false;
        }
    }

    uint8_t pos = (uint8_t)pcs.autovib_pos;
    int32_t table_val;
    switch (inst.vibrato_type) {
        case 1: table_val = (pos > 127) ? 64 : -64; break;                    // square
        case 2: table_val = (int32_t)(((pos >> 1) + 64) & 127) - 64; break;   // ramp
        case 3: table_val = (int32_t)((-(pos >> 1) + 64) & 127) - 64; break;  // ramp, inverted
        default: table_val = (int32_t)TRACKER_AUTOVIB_SINE[pos]; break;       // sine
    }

    double delta = (double)table_val * pcs.autovib_amp / 256.0;
    pcs.autovib_pos = (pcs.autovib_pos + inst.vibrato_rate) & 0xFFu;
    return delta;
}

// Result of mapping (instrument, note) through the sample-map/sample-index
// tables -- shared by a normal trigger and a tone-portamento retarget (which
// needs the destination note's tuning without actually triggering it).
struct TrackerSampleResolution {
    bool ok;
    uint32_t global_sample;
    const SampleHeader *sh;
    double base_note;  // sh->relative_note + (note - 1)
};

inline TrackerSampleResolution tracker_resolve_note_sample(const SongHeader *song, uint8_t instrument, uint8_t note) {
    TrackerSampleResolution r{false, 0, nullptr, 0.0};
    if (note < 1 || note > 96) return r;
    if (instrument == 0 || instrument > song->num_instruments) return r;

    const InstrumentHeader &inst = tracker_instrument_table(song)[instrument - 1];
    if (inst.sample_map_offset == 0) return r;

    const uint8_t *base = tracker_blob_base(song);
    uint8_t local = base[inst.sample_map_offset + (note - 1)];
    if (local == 0xFF || local >= inst.num_samples) return r;

    const uint32_t *sample_index = reinterpret_cast<const uint32_t *>(base + inst.sample_index_offset);
    uint32_t global_sample = sample_index[local];
    if (global_sample >= song->num_samples) return r;

    r.ok = true;
    r.global_sample = global_sample;
    r.sh = &tracker_sample_table(song)[global_sample];
    r.base_note = (double)r.sh->relative_note + (double)(note - 1);
    return r;
}

// Resolves one channel's note trigger (tick 0 of a row only) against the
// instrument/sample tables and updates `pcs` in place. No-op if there's no
// note, no instrument on record for this channel, or the instrument's
// sample_map has no sample mapped to this note (0xFF) -- all silently
// leave the channel exactly as it was, matching FT2's "nothing to play"
// behaviour rather than cutting a note that just has no data behind it.
// Never called for a tone-portamento row with a note -- that's a retarget,
// not a retrigger (see tracker_process_effects_tick0()). Key-off (note 97)
// does not touch volume directly -- it only marks `key_off`, which
// tracker_resolve_envelope_volpan()/tracker_fadeout_tick() consume every
// tick from here on to release the envelope/fadeout naturally.
inline void tracker_trigger_note(const SongHeader *song, const Event &ev,
                                  PlayerChannelState &pcs, ChannelTick &ct, bool linear) {
    if (ev.instrument != 0) pcs.instrument = ev.instrument;

    if (ev.note == 97) {  // key off
        pcs.key_off = true;
        ct.flags |= TICK_KEY_OFF;
        return;
    }
    if (ev.note < 1 || ev.note > 96) return;

    TrackerSampleResolution res = tracker_resolve_note_sample(song, pcs.instrument, ev.note);
    if (!res.ok) return;
    const SampleHeader &sh = *res.sh;

    // 9xx sample offset. Memory is written here unconditionally (any
    // 9xx next to this note updates it, even if the resulting offset turns
    // out to be past the sample's end below) -- only a 9xx with *no* note on
    // its row leaves the memory untouched, which is why this lives inside
    // the successful-trigger path rather than being decoded up front.
    uint32_t start_pos = 0;
    if ((Effect)ev.effect == Effect::SAMPLE_OFFSET) {
        if (ev.effect_param != 0) pcs.sample_offset_memory = ev.effect_param;
        uint32_t offset_samples = (uint32_t)pcs.sample_offset_memory * 256u;
        // FT2/openmpt123: an offset at or past the sample's length silently
        // suppresses the whole trigger (verified against openmpt123 -- "notes
        // with offset commands beyond the sample length are never
        // triggered") rather than clamping to the end or wrapping. The
        // channel is left exactly as tracker_resolve_note_sample()'s other
        // "nothing to play" cases leave it -- pcs.instrument above already
        // latched, nothing else touched.
        if (offset_samples >= sh.length) return;
        start_pos = offset_samples << TRACKER_POS_FRAC_BITS;
    }

    const uint8_t *base = tracker_blob_base(song);
    const uint32_t *incs = reinterpret_cast<const uint32_t *>(base + sh.note_increments_offset);
    pcs.inc = incs[ev.note - 1];
    pcs.sample_id = res.global_sample;
    pcs.base_note = res.base_note;
    pcs.finetune = (double)sh.finetune;
    pcs.period = tracker_note_to_period(pcs.base_note, pcs.finetune, linear);
    pcs.vibrato_pos = 0;  // new note resets vibrato phase (no E4 "keep position" support yet)

    pcs.vol64 = sh.default_volume > 64 ? 64 : sh.default_volume;
    pcs.pan_xm = (uint32_t)sh.default_panning;
    // Vol-column overrides of the above (SET_VOLUME/SET_PANNING) are applied
    // right after this call returns, by tracker_process_vol_column_tick0()
    // -- uniformly, whether or not this row triggered a note.

    pcs.key_off = false;
    pcs.vol_env_pos = 0;
    pcs.pan_env_pos = 0;
    pcs.fadeout_vol = 32768.0;
    pcs.autovib_pos = 0;
    if (pcs.instrument != 0 && pcs.instrument <= song->num_instruments) {
        const InstrumentHeader &inst = tracker_instrument_table(song)[pcs.instrument - 1];
        if (inst.vibrato_depth > 0 && inst.vibrato_sweep > 0) {
            pcs.autovib_amp = 0.0;
            pcs.autovib_sweeping = true;
        } else {
            pcs.autovib_amp = (double)inst.vibrato_depth;
            pcs.autovib_sweeping = false;
        }
    }

    pcs.trigger++;
    ct.flags |= TICK_NOTE_ON;
    ct.start_pos = start_pos;
}

// Resolves the volume column's tick-0 behaviour against `pcs` -- called for
// every row_boundary tick, note or not, since the volume column can carry a
// standalone command (e.g. a volslide continuing under a row with no note).
// Instant commands (set volume/panning, fine slides) apply immediately;
// continuous commands (volslide, vol-column vibrato/panslide/tone-porta)
// latch into active_vol_effect/active_vol_param for ticks 1..speed-1
// (tracker_tick_period()/tracker_apply_tick_volume_effects()). The volume
// column is a second, independent effect slot from the pattern effect
// column -- XM allows both on the same row -- so it gets its own memory
// slots (vol_volslide_memory, vol_panslide_memory) rather than sharing the
// effect column's.
inline void tracker_process_vol_column_tick0(const Event &ev, PlayerChannelState &pcs) {
    VolEffect veff = (VolEffect)ev.vol_effect;
    uint8_t vp = ev.vol_param;

    switch (veff) {
        case VolEffect::SET_VOLUME: {
            uint32_t v = vp;
            if (v > 64) v = 64;
            pcs.vol64 = v;
            break;
        }
        case VolEffect::SET_PANNING:
            // Vol-column panning is a 4-bit field (0..15): byte-replicate to
            // the engine's 0..255 XM-convention domain, same endpoint-exact
            // expansion as tracker_xm_pan_to_q15() uses for the full 8-bit case.
            pcs.pan_xm = (uint32_t)(((vp & 0x0F) << 4) | (vp & 0x0F));
            break;
        case VolEffect::FINE_VOLSLIDE_DOWN: {
            int32_t v = (int32_t)pcs.vol64 - (int32_t)vp;
            pcs.vol64 = (uint32_t)(v < 0 ? 0 : v);
            break;
        }
        case VolEffect::FINE_VOLSLIDE_UP: {
            int32_t v = (int32_t)pcs.vol64 + (int32_t)vp;
            pcs.vol64 = (uint32_t)(v > 64 ? 64 : v);
            break;
        }
        case VolEffect::VOLSLIDE_DOWN:
        case VolEffect::VOLSLIDE_UP:
            if (vp != 0) pcs.vol_volslide_memory = vp;
            pcs.active_vol_effect = veff;
            pcs.active_vol_param = pcs.vol_volslide_memory;
            break;
        case VolEffect::SET_VIBRATO_SPEED:
            // A passive memory-setter, not itself continuous -- it does not
            // start the oscillator (matches FT2: this only ever primes the
            // speed a later VIBRATO command, from either column, will use).
            if (vp != 0) pcs.vibrato_speed = vp;
            break;
        case VolEffect::VIBRATO:
            if (vp != 0) pcs.vibrato_depth = vp;
            pcs.active_vol_effect = VolEffect::VIBRATO;
            break;
        case VolEffect::PANSLIDE_LEFT:
        case VolEffect::PANSLIDE_RIGHT:
            if (vp != 0) pcs.vol_panslide_memory = vp;
            pcs.active_vol_effect = veff;
            pcs.active_vol_param = pcs.vol_panslide_memory;
            break;
        case VolEffect::TONE_PORTA:
            // No memory reuse: the vol column's rate comes straight from
            // tracker_volcol_toneporta_rate(vp) every time it's restated, a
            // 0 param legitimately meaning "don't move this row" rather
            // than "reuse the last rate" (unlike the effect column's 3xx).
            pcs.active_vol_effect = VolEffect::TONE_PORTA;
            pcs.active_vol_param = vp;
            break;
        default:
            break;
    }
}

// Tick-0-only processing for one channel: note trigger (or tone-portamento
// retarget, which suppresses the trigger), the volume column, effect-memory
// resolution, and every effect that acts instantaneously rather than
// per-tick (Cxx, 8xx, Bxx, Dxx, Fxx). Sets `pcs.active_effect`/
// `active_param` for tracker_tick_period() to consume on ticks 1..speed-1 of
// this row -- resolved here (memory substituted) so later ticks never
// re-touch the memory slots. An event with no effect column entry correctly
// resets active_effect to NONE, which is what makes a continuous effect
// require restating every row it runs on (module_tracker.md: "tick-0-vs-later-
// tick semantics ... is where the real work is").
inline void tracker_process_effects_tick0(const SongHeader *song, PlayerState &st, uint32_t c,
                                           const Event &ev, ChannelTick &ct, bool linear) {
    PlayerChannelState &pcs = st.ch[c];
    Effect eff = (Effect)ev.effect;
    uint8_t param = ev.effect_param;
    VolEffect veff = (VolEffect)ev.vol_effect;

    pcs.active_effect = Effect::NONE;
    pcs.active_param = 0;
    pcs.active_vol_effect = VolEffect::NONE;
    pcs.active_vol_param = 0;

    // Tone portamento retargets rather than retriggers -- true whether it's
    // requested from the effect column (3xx) or the volume column (Fx), so
    // a note on this row must not click/reset the sample either way.
    bool tone_porta_row = (eff == Effect::TONE_PORTA) || (veff == VolEffect::TONE_PORTA);

    if (tone_porta_row) {
        // The instrument column, key-off, and vol column still behave normally.
        if (ev.instrument != 0) pcs.instrument = ev.instrument;
        if (ev.note == 97) {
            pcs.key_off = true;
            ct.flags |= TICK_KEY_OFF;
        } else if (ev.note >= 1 && ev.note <= 96) {
            TrackerSampleResolution res = tracker_resolve_note_sample(song, pcs.instrument, ev.note);
            if (res.ok) pcs.tone_porta_target = tracker_note_to_period(res.base_note, (double)res.sh->finetune, linear);
        }
    } else {
        tracker_trigger_note(song, ev, pcs, ct, linear);
    }

    tracker_process_vol_column_tick0(ev, pcs);

    if (eff == Effect::TONE_PORTA) {
        if (param != 0) pcs.tone_porta_memory = param;
        pcs.active_effect = Effect::TONE_PORTA;
        pcs.active_param = pcs.tone_porta_memory;
        return;
    }

    switch (eff) {
        case Effect::ARPEGGIO:
            pcs.active_effect = Effect::ARPEGGIO;
            pcs.active_param = param;  // no memory: 0-param arpeggio is normalized to NONE at decode time
            break;
        case Effect::PORTA_UP:
            if (param != 0) pcs.porta_up_memory = param;
            pcs.active_effect = Effect::PORTA_UP;
            pcs.active_param = pcs.porta_up_memory;
            break;
        case Effect::PORTA_DOWN:
            if (param != 0) pcs.porta_down_memory = param;
            pcs.active_effect = Effect::PORTA_DOWN;
            pcs.active_param = pcs.porta_down_memory;
            break;
        case Effect::VIBRATO: {
            uint8_t hi = param >> 4, lo = param & 0x0F;
            if (hi != 0) pcs.vibrato_speed = hi;
            if (lo != 0) pcs.vibrato_depth = lo;
            pcs.active_effect = Effect::VIBRATO;
            break;
        }
        case Effect::VOLUME_SLIDE:
            if (param != 0) pcs.volslide_memory = param;
            pcs.active_effect = Effect::VOLUME_SLIDE;
            pcs.active_param = pcs.volslide_memory;
            break;
        case Effect::SET_VOLUME: {
            uint32_t v = param;
            if (v > 64) v = 64;
            pcs.vol64 = v;
            break;
        }
        case Effect::SET_PANNING:
            // Effect-column 8xx uses the full byte 0..255 directly, unlike
            // the volume column's 4-bit Cxx field.
            pcs.pan_xm = (uint32_t)param;
            break;
        case Effect::POSITION_JUMP:
            st.jump_pending = true;
            st.jump_target_order = param;
            break;
        case Effect::PATTERN_BREAK:
            // XM inherits ProTracker's decimal-digit encoding for Dxx: the
            // param byte's nibbles are read as two decimal digits (tens,
            // units), not a straight binary row number.
            st.break_pending = true;
            st.break_row = (uint32_t)(param >> 4) * 10u + (uint32_t)(param & 0x0F);
            break;
        case Effect::SET_SPEED:
            st.speed = param;
            break;
        case Effect::SET_BPM:
            st.samples_per_tick = tracker_samples_per_tick(param);
            break;

        // Remaining Exy sub-commands.
        case Effect::FINE_PORTA_UP:
            // Applied once, here, at tick 0 -- unlike the continuous 1xx
            // (tracker_tick_period()'s PORTA_UP case), which reapplies every
            // tick of ticks 1..speed-1. Own memory slot: FT2 does not share
            // it with 1xx's.
            if (param != 0) pcs.fine_porta_up_memory = param;
            pcs.period -= (double)pcs.fine_porta_up_memory;
            tracker_clamp_period(pcs.period);
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            break;
        case Effect::FINE_PORTA_DOWN:
            if (param != 0) pcs.fine_porta_down_memory = param;
            pcs.period += (double)pcs.fine_porta_down_memory;
            tracker_clamp_period(pcs.period);
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            break;
        case Effect::FINE_VOLSLIDE_UP: {
            if (param != 0) pcs.fine_volslide_up_memory = param;
            int32_t v = (int32_t)pcs.vol64 + (int32_t)pcs.fine_volslide_up_memory;
            pcs.vol64 = (uint32_t)(v > 64 ? 64 : v);
            break;
        }
        case Effect::FINE_VOLSLIDE_DOWN: {
            if (param != 0) pcs.fine_volslide_down_memory = param;
            int32_t v = (int32_t)pcs.vol64 - (int32_t)pcs.fine_volslide_down_memory;
            pcs.vol64 = (uint32_t)(v < 0 ? 0 : v);
            break;
        }
        case Effect::NOTE_CUT:
            // Not instant -- fires later, on tick_in_row == param (0 cuts
            // immediately, on this same tick). tracker_apply_tick_note_cut()
            // consumes this every tick, including tick 0, since a param-0
            // cut must apply to the very trigger this row just produced.
            pcs.active_effect = Effect::NOTE_CUT;
            pcs.active_param = param;
            break;
        case Effect::RETRIG_NOTE:
            // Shared enum value for both the top-level Rxy effect (full
            // param byte: high nibble = volume-change type, low nibble =
            // tick interval) and the Exy-decomposed E9x (effects.py already
            // zeroes the high nibble when decoding E9x, so it always reaches
            // here as "interval only, no volume change" -- the same code
            // handles both correctly with no extra branching).
            pcs.active_effect = Effect::RETRIG_NOTE;
            pcs.active_param = param;  // no memory -- must restate every row, matching arpeggio's precedent
            break;
        case Effect::PATTERN_DELAY:
            // Row-level, like Bxx/Dxx above -- last channel wins if more
            // than one carries it (same left-to-right convention). Consumed
            // at the rollover in player_produce_tick(), not here: this row's
            // own tick 0 has already fully run by the time the rollover
            // decides whether to hold or advance.
            st.pattern_delay_remaining = param;
            break;
        default:
            break;
    }
}

// Straight-line glide from `period` toward `target` at `step` units/tick,
// clamping (not overshooting) once it arrives -- the shared shape of both
// the effect-column (3xx) and volume-column (Fx) tone portamento.
inline double tracker_glide_period(double period, double target, double step) {
    if (period < target) {
        period += step;
        if (period > target) period = target;
    } else if (period > target) {
        period -= step;
        if (period < target) period = target;
    }
    return period;
}

// One tick of the shared vibrato oscillator (effect-column 4xy and
// volume-column Bx both drive this same position/speed/depth state --
// tracker_process_vol_column_tick0()'s header comment). Empirically
// calibrated against openmpt123 (pitch-tracked a long held vibrato run):
// this table's "quarter cycle" (index 0 to its peak at 16) took roughly 22
// ticks at speed 1, closer to a >>1 position-to-index shift than the >>2
// some published FT2 pseudocode uses. Not chased to bit-exactness -- a
// continuous oscillation has no settling point to converge on the way
// porta/tone porta do, so any small rate mismatch is permanent phase drift.
inline double tracker_vibrato_delta(PlayerChannelState &pcs) {
    uint8_t idx = (pcs.vibrato_pos >> 1) & 0x1F;
    int32_t amplitude = (int32_t)TRACKER_VIBRATO_SINE[idx];
    int32_t delta = (amplitude * (int32_t)pcs.vibrato_depth) >> 5;
    if (pcs.vibrato_pos >= 128) delta = -delta;
    pcs.vibrato_pos = (uint8_t)(pcs.vibrato_pos + pcs.vibrato_speed);
    return (double)delta;
}

// Ticks 1..speed-1 of a row: advances/applies whichever continuous *pitch*
// effect is active this tick, from either column, and returns this tick's
// period (not yet converted to an increment -- the caller still needs to
// add autovibrato's delta on top, tracker_autovibrato_delta()). Porta
// up/down and tone portamento write the new pitch back into `pcs.period` --
// a lasting change that must survive into rows that don't restate the
// effect. Arpeggio and vibrato are transient: they compute an offset from
// `pcs.period` for this tick only and never touch it, so the channel
// returns to its unmodulated pitch the instant the effect isn't restated.
// The volume column's own TONE_PORTA/VIBRATO only apply here when the
// effect column isn't *already* driving the same mechanism this tick --
// XM's two columns are logically independent, but both driving one glide/
// oscillator at once is a pathological, essentially never-authored case,
// and letting the effect column win avoids double-stepping it.
inline double tracker_tick_period(PlayerChannelState &pcs, uint32_t tick_in_row, bool linear) {
    double period = pcs.period;

    switch (pcs.active_effect) {
        case Effect::ARPEGGIO: {
            uint32_t which = tick_in_row % 3;
            if (which != 0) {
                // Verified against openmpt123: tick 1 of the cycle is the
                // *low* nibble's offset, tick 2 the high nibble's -- base,
                // +y, +x, not the other way round.
                int offset = (which == 1) ? (pcs.active_param & 0x0F) : (pcs.active_param >> 4);
                period = tracker_note_to_period(pcs.base_note + offset, pcs.finetune, linear);
            }
            break;
        }
        // No *4 here: some published FT2 pseudocode multiplies the raw
        // param by 4 for linear-frequency slides, but that's compensating
        // for a period scale (64 units/semitone) this engine doesn't use --
        // periods.py's linear period is already 16 units/semitone (see
        // TRACKER_LINEAR_BASE_PERIOD's comment), so applying that *4 here
        // on top would double the compensation. Verified against openmpt123
        // by pitch-tracking a held porta-up run.
        case Effect::PORTA_UP:
            pcs.period -= (double)pcs.active_param;
            tracker_clamp_period(pcs.period);
            period = pcs.period;
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            break;
        case Effect::PORTA_DOWN:
            pcs.period += (double)pcs.active_param;
            tracker_clamp_period(pcs.period);
            period = pcs.period;
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            break;
        case Effect::TONE_PORTA:
            pcs.period = tracker_glide_period(pcs.period, pcs.tone_porta_target, (double)pcs.active_param);
            period = pcs.period;
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            break;
        case Effect::VIBRATO:
            period = pcs.period + tracker_vibrato_delta(pcs);
            break;
        default:
            break;
    }

    // pcs.inc is kept in sync with pcs.period whenever a *persisting* glide
    // (porta/tone-porta, from either column) moves it, above and here --
    // that's what lets the caller's fast path (player_produce_tick(): reuse
    // pcs.inc when `base_period == pcs.period`) stay correct on a later,
    // effect-less tick: the pitch change from an unrestated portamento must
    // survive, only the *effect application* needs restating every row.
    // Arpeggio and vibrato are the opposite -- transient, so they must NOT
    // write pcs.inc, or the modulation would leak into ticks that never
    // asked for it.
    if (pcs.active_vol_effect == VolEffect::TONE_PORTA && pcs.active_effect != Effect::TONE_PORTA) {
        double rate = tracker_volcol_toneporta_rate(pcs.active_vol_param);
        pcs.period = tracker_glide_period(pcs.period, pcs.tone_porta_target, rate);
        period = pcs.period;
        pcs.inc = tracker_period_to_inc(pcs.period, linear);
    } else if (pcs.active_vol_effect == VolEffect::VIBRATO && pcs.active_effect != Effect::VIBRATO) {
        period = pcs.period + tracker_vibrato_delta(pcs);
    }

    return period;
}

// Ticks 1..speed-1 of a row: the *non*-pitch continuous effects (volume
// slides, panning slides) from either column. Split out from
// tracker_tick_period() because these touch vol64/pan_xm, not period/inc.
inline void tracker_apply_tick_volume_effects(PlayerChannelState &pcs) {
    if (pcs.active_effect == Effect::VOLUME_SLIDE) {
        uint8_t hi = pcs.active_param >> 4, lo = pcs.active_param & 0x0F;
        int32_t v = (int32_t)pcs.vol64;
        if (hi != 0) v += hi;
        else v -= lo;
        if (v < 0) v = 0;
        if (v > 64) v = 64;
        pcs.vol64 = (uint32_t)v;
    }

    if (pcs.active_vol_effect == VolEffect::VOLSLIDE_DOWN) {
        int32_t v = (int32_t)pcs.vol64 - (int32_t)pcs.active_vol_param;
        pcs.vol64 = (uint32_t)(v < 0 ? 0 : v);
    } else if (pcs.active_vol_effect == VolEffect::VOLSLIDE_UP) {
        int32_t v = (int32_t)pcs.vol64 + (int32_t)pcs.active_vol_param;
        pcs.vol64 = (uint32_t)(v > 64 ? 64 : v);
    }

    if (pcs.active_vol_effect == VolEffect::PANSLIDE_LEFT) {
        int32_t p = (int32_t)pcs.pan_xm - (int32_t)pcs.active_vol_param;
        pcs.pan_xm = (uint32_t)(p < 0 ? 0 : p);
    } else if (pcs.active_vol_effect == VolEffect::PANSLIDE_RIGHT) {
        int32_t p = (int32_t)pcs.pan_xm + (int32_t)pcs.active_vol_param;
        pcs.pan_xm = (uint32_t)(p > 255 ? 255 : p);
    }
}

// ECx: fires exactly once, on the tick within the row that matches
// active_param (0 cuts on the trigger tick itself) -- checked every tick,
// including tick 0, unlike the other continuation effects above which only
// ever run on ticks 1..speed-1. tick_in_row only ever increases within a
// row, so this can't refire later in the same row once it's matched, and
// active_effect is reset to NONE at the next row's own tick 0 regardless.
inline void tracker_apply_tick_note_cut(PlayerChannelState &pcs, ChannelTick &ct, uint32_t tick_in_row) {
    if (pcs.active_effect == Effect::NOTE_CUT && tick_in_row == pcs.active_param) {
        pcs.vol64 = 0;
        ct.flags |= TICK_NOTE_CUT;
    }
}

// E9x/Rxy retrigger volume-change table, keyed by the param's high
// nibble -- standard XM/FT2 table (additive for 1-5/9-D, multiplicative for
// 6/7/E/F, no-op for 0/8). E9x always reaches here with this nibble zeroed
// (effects.py's decode strips it), so the table's 0x0 "no change" entry is
// what makes E9x's plain fixed-interval retrigger fall out of the same code
// as Rxy's fuller form for free.
inline void tracker_retrig_apply_volume(PlayerChannelState &pcs, uint8_t volchg) {
    int32_t v = (int32_t)pcs.vol64;
    switch (volchg) {
        case 0x1: v -= 1; break;
        case 0x2: v -= 2; break;
        case 0x3: v -= 4; break;
        case 0x4: v -= 8; break;
        case 0x5: v -= 16; break;
        case 0x6: v = (v * 2) / 3; break;
        case 0x7: v = v / 2; break;
        case 0x9: v += 1; break;
        case 0xA: v += 2; break;
        case 0xB: v += 4; break;
        case 0xC: v += 8; break;
        case 0xD: v += 16; break;
        case 0xE: v = (v * 3) / 2; break;
        case 0xF: v = v * 2; break;
        default: break;  // 0x0, 0x8 -- no change
    }
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    pcs.vol64 = (uint32_t)v;
}

// E9x/Rxy: ticks 1..speed-1 only (tick 0 already got this row's own
// natural trigger, if any -- matching arpeggio/vibrato's tick-0 exclusion).
// Retriggers the *currently playing* sample from position 0 at its current
// pitch -- no note/instrument re-resolution, just the same generation-bump/
// TICK_NOTE_ON/start_pos=0 shape tracker_trigger_note() ends with.
inline void tracker_apply_tick_retrigger(PlayerChannelState &pcs, ChannelTick &ct, uint32_t tick_in_row) {
    if (pcs.active_effect != Effect::RETRIG_NOTE || tick_in_row == 0) return;
    uint8_t interval = pcs.active_param & 0x0F;
    if (interval == 0 || tick_in_row % interval != 0) return;
    tracker_retrig_apply_volume(pcs, pcs.active_param >> 4);
    pcs.trigger++;
    ct.flags |= TICK_NOTE_ON;
    ct.start_pos = 0;
}

// Runs every tick (tick 0 included, independent of any pattern effect):
// advances the volume/panning envelopes and fadeout, and resolves the
// result into `pcs.volL`/`pcs.volR` (Q15, post-pan) -- the FT2 formula this
// ports is `updateVolPanAutoVib()`. The panning envelope's
// effect is deliberately asymmetric: `pan_mul` shrinks toward 0 as the
// channel's base pan approaches either extreme, so the envelope can't push
// panning further out than the channel's own pan already allows -- centered
// channels get the envelope's full swing, hard-panned ones get almost none.
inline void tracker_resolve_envelope_volpan(const SongHeader *song, const InstrumentHeader &inst,
                                             PlayerChannelState &pcs) {
    const uint8_t *base = tracker_blob_base(song);

    bool vol_env_enabled = (inst.vol_env_flags & TRACKER_ENV_ENABLED) && inst.vol_env_count > 0;
    double vol_env_scale = 1.0;
    if (vol_env_enabled) {
        const EnvelopePoint *pts = reinterpret_cast<const EnvelopePoint *>(base + inst.vol_env_offset);
        double v = tracker_envelope_tick(pts, inst.vol_env_count, inst.vol_env_flags,
                                          inst.vol_env_sustain, inst.vol_env_loop_start, inst.vol_env_loop_end,
                                          pcs.vol_env_pos, pcs.key_off);
        vol_env_scale = v / 64.0;
        tracker_fadeout_tick(inst, pcs);
    } else if (pcs.key_off) {
        // Fadeout is a *release* mechanism for an envelope-driven
        // instrument -- verified against openmpt123: an instrument
        // with no volume envelope at all cuts at key-off almost instantly,
        // regardless of its fadeout field's value, rather than fading over
        // 32768/fadeout ticks. Reusing fadeout_vol for the cut (instead of
        // vol64 directly) keeps this reversible if the channel retriggers
        // mid-cut and keeps the single downstream vol_scale formula below
        // as the only place that reads it.
        pcs.fadeout_vol = 0.0;
    }

    double final_pan_xm = (double)pcs.pan_xm;
    if ((inst.pan_env_flags & TRACKER_ENV_ENABLED) && inst.pan_env_count > 0) {
        const EnvelopePoint *pts = reinterpret_cast<const EnvelopePoint *>(base + inst.pan_env_offset);
        double v = tracker_envelope_tick(pts, inst.pan_env_count, inst.pan_env_flags,
                                          inst.pan_env_sustain, inst.pan_env_loop_start, inst.pan_env_loop_end,
                                          pcs.pan_env_pos, pcs.key_off);
        double pan_mul = (128.0 - std::fabs((double)pcs.pan_xm - 128.0)) * 8.0;
        final_pan_xm = (double)pcs.pan_xm + (v - 32.0) * pan_mul / 256.0;
        if (final_pan_xm < 0.0) final_pan_xm = 0.0;
        if (final_pan_xm > 255.0) final_pan_xm = 255.0;
    }

    double vol_scale = (pcs.vol64 / 64.0) * vol_env_scale * (pcs.fadeout_vol / 32768.0);
    if (vol_scale < 0.0) vol_scale = 0.0;
    if (vol_scale > 1.0) vol_scale = 1.0;
    int32_t vol_q15 = (int32_t)(vol_scale * 32767.0 + 0.5);

    int16_t pan_q15 = tracker_xm_pan_to_q15((uint32_t)(final_pan_xm + 0.5));
    int32_t gain_l, gain_r;
    pan_gains_q15(pan_q15, gain_l, gain_r);
    pcs.volL = (vol_q15 * gain_l) >> 15;
    pcs.volR = (vol_q15 * gain_r) >> 15;
}

// Advances the player by exactly one tick and fills `out` with that tick's
// per-channel state. Pure function of `st` + the blob -- no I/O, no
// allocation -- so a host harness and Core 0 can call it identically.
// Returns true on the tick where the order list wraps back to the restart
// position, i.e. "one full pass just completed" -- device playback ignores
// this and loops forever; a host renderer uses it as its natural stop
// condition.
inline bool player_produce_tick(PlayerState &st, const SongHeader *song, TickBlock &out) {
    const uint32_t num_channels = song->num_channels;
    const bool linear = song->freq_table != 0;
    const uint8_t *orders = tracker_order_table(song);
    uint32_t pat_idx = orders[st.order_idx];
    const PatternHeader &pat = tracker_pattern_table(song)[pat_idx];

    // EEx: a pattern-delay held repeat reaches tick_in_row == 0 again
    // (the row doesn't advance), but must not be treated as a genuine new
    // row -- pattern_delay_holding (set at the previous call's rollover,
    // below) tells the two apart. Every per-channel dispatch below keys off
    // this adjusted `row_boundary`, not the raw tick_in_row == 0 check, so a
    // held repeat's own tick 0 falls through to the normal *continuation*
    // path (ticks 1..speed-1's effects) for free -- exactly what EEx needs
    // (the row's trigger/tick-0 effects apply once, on the genuine pass
    // only; held repeats just keep whatever was already running).
    bool row_boundary = (st.tick_in_row == 0) && !st.pattern_delay_holding;
    for (uint32_t c = 0; c < TRACKER_MAX_CHANNELS; c++) {
        ChannelTick &ct = out.ch[c];
        ct.flags = 0;
        ct.start_pos = 0;
        if (c >= num_channels) continue;

        PlayerChannelState &pcs = st.ch[c];
        double base_period;
        if (row_boundary) {
            const Event &ev = tracker_event_at(song, pat, st.row, c, num_channels);
            // EDx: a nonzero note-delay param defers this channel's
            // *entire* tick-0 processing (trigger, volume column, every
            // other tick-0-only effect -- EDx occupies the whole effect
            // column, so none of those can coexist with it on this cell
            // anyway) to the tick within the row that matches. Until then,
            // the channel does nothing new: active_effect/active_vol_effect
            // are cleared so no leftover continuation from the *previous*
            // row keeps running into this one.
            uint8_t delay = ((Effect)ev.effect == Effect::NOTE_DELAY) ? ev.effect_param : 0;
            if (delay != 0) {
                pcs.note_delay_tick = delay;
                pcs.active_effect = Effect::NONE;
                pcs.active_vol_effect = VolEffect::NONE;
            } else {
                pcs.note_delay_tick = 0xFF;
                tracker_process_effects_tick0(song, st, c, ev, ct, linear);
            }
            base_period = pcs.period;  // matches the precomputed table entry pcs.inc already holds
        } else if (pcs.note_delay_tick == st.tick_in_row) {
            // The delayed trigger fires now -- re-fetch the same row's event
            // and process it exactly as a fresh tick 0 would.
            const Event &ev = tracker_event_at(song, pat, st.row, c, num_channels);
            tracker_process_effects_tick0(song, st, c, ev, ct, linear);
            pcs.note_delay_tick = 0xFF;
            base_period = pcs.period;
        } else if (pcs.note_delay_tick != 0xFF) {
            // Still waiting for this row's delay tick -- nothing new.
            base_period = pcs.period;
        } else {
            tracker_apply_tick_volume_effects(pcs);
            tracker_apply_tick_retrigger(pcs, ct, st.tick_in_row);
            base_period = tracker_tick_period(pcs, st.tick_in_row, linear);
        }
        tracker_apply_tick_note_cut(pcs, ct, st.tick_in_row);

        // Instrument envelopes/fadeout/autovibrato run every tick,
        // independent of row_boundary or any pattern effect. When nothing
        // modulated the pitch this tick -- no pattern/vol-column pitch
        // effect (base_period came back exactly equal to pcs.period, a
        // plain reassignment with no arithmetic in that case) and no
        // autovibrato (by far the common case for both) -- reuse pcs.inc
        // as-is rather than paying a tracker_period_to_inc() round-trip:
        // exactly what tracker_trigger_note()'s precomputed note_increments
        // table latched on tick 0.
        double autovib_delta = 0.0;
        bool has_inst = pcs.instrument != 0 && pcs.instrument <= song->num_instruments;
        const InstrumentHeader *inst_hdr = nullptr;
        if (has_inst) {
            inst_hdr = &tracker_instrument_table(song)[pcs.instrument - 1];
            autovib_delta = tracker_autovibrato_delta(*inst_hdr, pcs);
        }

        if (base_period == pcs.period && autovib_delta == 0.0) {
            ct.inc = pcs.inc;
        } else {
            ct.inc = tracker_period_to_inc(base_period + autovib_delta, linear);
        }

        if (has_inst) tracker_resolve_envelope_volpan(song, *inst_hdr, pcs);

        ct.tgt_volL = pcs.volL;
        ct.tgt_volR = pcs.volR;
        ct.sample_id = pcs.sample_id;
        ct.trigger = pcs.trigger;
    }

    // Fxx (tick 0, above) must be visible in the block it belongs to --
    // module_tracker.md: "Fxx tempo changes have to take effect at the tick
    // boundary they belong to" -- so this reads st.samples_per_tick after
    // the per-channel loop, not before it.
    out.samples_per_tick = st.samples_per_tick;

    bool looped = false;
    st.tick_in_row++;
    if (st.tick_in_row >= st.speed) {
        st.tick_in_row = 0;
        if (st.pattern_delay_remaining > 0) {
            // EEx: hold the current row for one more full-speed pass --
            // row/order untouched, and the *next* call's tick_in_row == 0 is
            // flagged as a held repeat (this function's row_boundary
            // computation, top) rather than a genuine new row, so it won't
            // re-trigger. Bxx/Dxx pending on this same row are deliberately
            // left pending rather than consumed here -- they still resolve
            // once the delay finally runs out, same as XM/FT2.
            st.pattern_delay_remaining--;
            st.pattern_delay_holding = true;
        } else {
            st.pattern_delay_holding = false;
            if (st.jump_pending || st.break_pending) {
                uint32_t next_order = st.jump_pending ? st.jump_target_order : st.order_idx + 1;
                uint32_t next_row = st.break_pending ? st.break_row : 0;
                st.jump_pending = false;
                st.break_pending = false;
                if (next_order >= song->num_orders) {
                    next_order = song->restart_order;
                    looped = true;
                }
                st.order_idx = next_order;
                const uint32_t new_pat_idx = orders[st.order_idx];
                const PatternHeader &new_pat = tracker_pattern_table(song)[new_pat_idx];
                st.row = (next_row < new_pat.num_rows) ? next_row : 0;
            } else {
                st.row++;
                if (st.row >= pat.num_rows) {
                    st.row = 0;
                    st.order_idx++;
                    if (st.order_idx >= song->num_orders) {
                        st.order_idx = song->restart_order;
                        looped = true;
                    }
                }
            }
        }
    }
    return looped;
}

// Builds a small SRAM-resident TrackerSample descriptor per song sample,
// read once at song-load time -- the only place that ever dereferences
// SampleHeader (flash-resident on device). module_tracker.md's rationale for
// putting the player on Core 0 is explicitly to keep pattern-data flash
// reads off Core 1 ("never thrashes the XIP cache"); tracker_apply_tick()
// below only ever indexes the array this produces, never `song` itself, so
// Core 1's hot path stays flash-free.
//
// `sample_data_base`/`sample_data_base_offset` let one function serve both
// callers: the host harness has the whole blob resident already (base =
// blob base, offset = 0, since SampleHeader.data_offset is already
// blob-base-relative), while the device copies just the sample-data region
// into its own SRAM buffer (base = that buffer, offset =
// song->sample_data_offset, to rebase the blob-relative offset into the
// copy). `out` must have room for song->num_samples entries.
inline void tracker_build_resident_samples(const SongHeader *song, const int8_t *sample_data_base,
                                            uint32_t sample_data_base_offset, TrackerSample *out) {
    const SampleHeader *samples = tracker_sample_table(song);
    for (uint32_t i = 0; i < song->num_samples; i++) {
        const SampleHeader &sh = samples[i];
        out[i] = TrackerSample{
            sample_data_base + (sh.data_offset - sample_data_base_offset),
            sh.length, sh.loop_start, sh.loop_end, (uint8_t)sh.loop_type,
        };
    }
}

// Consumes one produced TickBlock into the mixer's per-channel voice state
// -- module_tracker.md's render-loop pseudocode calls this apply_tick(). Pure
// function of `tb` and `resident_samples` (built once by
// tracker_build_resident_samples() above): no SongHeader/flash access, so
// it's safe to call from Core 1's real-time render path. `voice_sample_desc`
// is per-channel scratch the caller owns for the lifetime of `voices` --
// TrackerVoice::sample is a pointer, so it needs somewhere stable to point
// that isn't the TickBlock (which the ring will overwrite next tick).
inline void tracker_apply_tick(const TickBlock &tb, const TrackerSample *resident_samples,
                                TrackerVoice *voices, TrackerSample *voice_sample_desc,
                                uint32_t num_channels) {
    for (uint32_t c = 0; c < num_channels; c++) {
        const ChannelTick &ct = tb.ch[c];
        TrackerVoice &v = voices[c];
        if (ct.flags & TICK_NOTE_ON) {
            voice_sample_desc[c] = resident_samples[ct.sample_id];
            v.sample = &voice_sample_desc[c];
            v.pos = ct.start_pos;
            v.inc = tracker_latch_inc(ct.inc);
            v.active = true;
            // Every real trigger starts a ping-pong sample playing
            // forward, even one retargeted mid-loop-region by a 9xx offset
            // (start_pos) -- FT2/openmpt123 never trigger a note already
            // mid-bounce.
            v.backward = false;
        } else if (v.active) {
            v.inc = tracker_latch_inc(ct.inc);
            // ct.inc == 0 means this channel hasn't been (re)triggered since
            // whatever produced this PlayerState -- most notably
            // tracker_transport_seek()'s player_init(), which resets every
            // channel's pcs.inc to 0 but has no way to reach into Core 1's
            // `voices` here and clear a voice a *previous* run left active.
            // Without this, a channel that isn't retriggered on the very
            // first row after a seek keeps v.active == true with v.inc == 0
            // -- exactly the state mixer.h's samples_to_loop_end() documents
            // as impossible ("a silent voice is active = false, never inc ==
            // 0"): pos never reaches end_pos through a zero increment, so
            // wrap_loop() never fires either, and mix_voice()'s `while (n >
            // 0)` spins forever on Core 1 (100% duty, frozen UI, silence --
            // exactly what stop-then-restart reproduced).
            if (v.inc == 0) v.active = false;
        }
        v.tgt_volL = ct.tgt_volL;
        v.tgt_volR = ct.tgt_volR;
    }
}
