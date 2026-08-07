#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "audio_common.h"
#include "blob_format.h"
#include "mixer.h"
#include "pan.h"

// Core 0 tracker player (#17/#18 landed the notes-only walk; #19 adds the
// 90%-usage effect set) — order-list/pattern walk, note triggering, the
// TickBlock/TickRing shapes from the design doc, plus per-tick effect state
// machines: arpeggio, porta up/down, tone porta, vibrato, volume slide, set
// volume, position jump, pattern break, set speed/tempo. Pure integer/double
// math over a `SongHeader*` blob, no pico-sdk: this header is included by
// both tools/host_render/render_xm_device.cpp (the #17 reference-diff
// harness) and the real Core 0 engine (player_task.cpp). `player_produce_tick()`
// is a pure function of `PlayerState` plus the blob -- identical behaviour on
// host or device is the whole point (tracker.md: "Any divergence between the
// host and device render paths defeats the purpose").
//
// Effect column commands NOT in #19's 90% set (E-subcommands, tremolo,
// retrig, tremor, global volume, panning slide, ...) are still no-ops here --
// tracker.md's "long tail of FT2 quirks" (#20+). The volume column's
// SET_VOLUME/SET_PANNING are honoured at trigger time (and on a
// tone-portamento row that omits a retrigger) because they aren't per-tick
// effects. Key-off (note 97) cuts the voice's volume target to 0 immediately
// rather than releasing an envelope -- there is no envelope yet (#20); this
// is the pre-envelope approximation.
//
// Requires osc_init_sine() to have been called first (pan_gains_q15's
// quadrature source), same precondition as mixer.h's consumers.

// Ring depth: tracker.md open question 1, resolved here. 2 slots is
// sufficient given 20ms of tick slack per tracker.md's own reasoning; a host
// harness driving this synchronously doesn't need lookahead at all, and #18
// inherits this constant as-is for the real cross-core case. This struct's
// head/tail bookkeeping is NOT atomic/barrier-safe -- cross-core memory
// ordering is hardware-specific and stays #18's job to add once Core 0 and
// Core 1 actually run this split across cores.
static constexpr uint32_t TICK_RING_DEPTH = 2;

// Fixed at 32 regardless of a given song's num_channels (2-32): TickBlock is
// sized for the format's ceiling, matching engine.h's MAX_VOICES. Kept as a
// local constant rather than including engine.h, which drags in
// engine_base.h -> pico-sdk headers this host-buildable file must not touch.
static constexpr uint32_t TRACKER_MAX_CHANNELS = 32;

enum ChannelTickFlags : uint8_t {
    TICK_NOTE_ON  = 0x01,  // (re)triggered this tick -- reset position, latch inc
    TICK_KEY_OFF  = 0x02,  // note-off (XM note 97) -- pre-envelope: volume target cut to 0
    TICK_NOTE_CUT = 0x04,  // reserved for a future issue's ECx (Note Cut); never set by this player
};

// One channel's state as of this tick. Restated every tick, not just on
// change -- tracker.md's render loop pseudocode re-applies whatever the
// latest TickBlock says unconditionally ("apply_tick(tick): latch
// inc/targets/triggers"), so the consumer never needs its own "did this
// change" logic. `trigger` is what lets it tell a restated-but-unchanged
// note apart from a genuine retrigger.
struct ChannelTick {
    uint32_t inc;                 // Q8.24, 0 = channel silent
    int32_t tgt_volL, tgt_volR;   // Q15, post-pan
    uint32_t sample_id;           // global sample index (SongHeader's sample table)
    uint32_t start_pos;           // Q18.14; always 0 until #21's 9xx sample offset
    uint8_t trigger;              // generation counter, bumped on note-on
    uint8_t flags;                // ChannelTickFlags
};

struct TickBlock {
    uint32_t samples_per_tick;
    ChannelTick ch[TRACKER_MAX_CHANNELS];
};

// Single-producer/single-consumer ring, genuinely cross-core safe (#18):
// Core 0 is the sole writer of `head`, Core 1 the sole writer of `tail`.
// `push()`/`pop()` release-store their own index; `full()`/`empty()`
// acquire-load the *other* index before touching a slot, so the producer's
// slot write happens-before the consumer sees `head` advance, and the
// consumer's slot read happens-before the producer reuses that slot for a
// new write. std::atomic (rather than hand-rolled ARM barriers, cf.
// engine_base.h's ParamExchangeT) because this file has to stay
// host-buildable -- tools/host_render links it with the host compiler, no
// pico-sdk headers allowed. Used single-threaded by the host harness today;
// real cross-core use starts with #18's Core 0 player task / Core 1 mixer.
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
// and (#19) the per-tick effect state machines -- pitch (`period`, the
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
    // via tracker_recompute_volume() every time either changes (trigger,
    // Cxx, Axy, vol-column set-volume/panning).
    uint32_t vol64 = 0;    // 0..64, XM convention
    int16_t pan_q15 = 0;

    // Pitch source of truth. `period` is in the song's native period units
    // (linear or Amiga, tracker.md/periods.py convention) -- portamento and
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
    uint8_t vibrato_speed = 0;
    uint8_t vibrato_depth = 0;
    uint8_t vibrato_pos = 0;  // 0..255, advances by vibrato_speed each active tick

    // Resolved once at tick 0 from this row's event (memory already
    // substituted for a zero param), consumed by every later tick of the
    // row. An XM continuous effect must be restated every row it runs on --
    // an empty effect column here correctly clears this to NONE, stopping
    // whatever was running.
    Effect active_effect = Effect::NONE;
    uint8_t active_param = 0;
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
};

// tracker.md "Fxx tempo changes ... samples_per_tick": 44100 * 2.5 / bpm,
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

// --- Period/frequency math (#19): a runtime C++ port of
// tools/xm2t00t/periods.py's linear/Amiga formulas. periods.py's own table
// (SampleHeader.note_increments_offset) only covers the no-effects-active
// case -- computed once, offline, per (sample, note). Portamento, tone
// portamento, vibrato and arpeggio modulate pitch continuously tick-by-tick,
// which needs the formula itself on-device, not just its precomputed table.
// Deliberately the same double-precision formulas as the host converter
// (not a fixed-point or table-based reimplementation) so a channel with no
// active pitch effect still lands on exactly the note_increments table's
// Q8.24 value -- see tracker_apply_pitch_vol_effect()'s callers, which only
// ever call into this math when an effect is actually modulating pitch this
// tick. 32 channels x a few effects x ~50Hz is nowhere near enough double
// math to dent Core 0's budget (tracker.md: "three million cycles per 20ms
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

// tracker.md "Fixed-Point Formats": inc = f_note / f_mix, Q8.24, rounded to
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
// limit (that level of precision is the "deep quirk tail", #19's own issue
// text), just a floor/ceiling so a long chain of unclamped portamento can't
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

// Rebuilds volL/volR (Q15, post-pan) from a channel's vol64/pan_q15 --
// called after a trigger and after any effect that changes volume or pan
// (Cxx, Axy, vol-column set-volume/panning).
inline void tracker_recompute_volume(PlayerChannelState &pcs) {
    int32_t vol_q15 = (int32_t)((pcs.vol64 * 32767u + 32u) / 64u);
    int32_t gain_l, gain_r;
    pan_gains_q15(pcs.pan_q15, gain_l, gain_r);
    pcs.volL = (vol_q15 * gain_l) >> 15;
    pcs.volR = (vol_q15 * gain_r) >> 15;
}

// Applies the vol column's direct sets against the channel's *current*
// vol64/pan_q15 (as opposed to tracker_trigger_note()'s use of the sample's
// defaults as the base) -- the path a tone-portamento row without a
// retrigger takes, since XM still honours vol-column commands there even
// though the note itself doesn't restart.
inline void tracker_apply_vol_column(const Event &ev, PlayerChannelState &pcs) {
    if ((VolEffect)ev.vol_effect == VolEffect::SET_VOLUME) {
        uint32_t v = ev.vol_param;
        if (v > 64) v = 64;
        pcs.vol64 = v;
    } else if ((VolEffect)ev.vol_effect == VolEffect::SET_PANNING) {
        uint32_t p4 = ev.vol_param & 0xF;
        pcs.pan_q15 = (int16_t)(int32_t)((p4 * 65535u / 15u) - 32768u);
    }
    tracker_recompute_volume(pcs);
}

// Resolves one channel's note trigger (tick 0 of a row only) against the
// instrument/sample tables and updates `pcs` in place. No-op if there's no
// note, no instrument on record for this channel, or the instrument's
// sample_map has no sample mapped to this note (0xFF) -- all silently
// leave the channel exactly as it was, matching FT2's "nothing to play"
// behaviour rather than cutting a note that just has no data behind it.
// Never called for a tone-portamento row with a note -- that's a retarget,
// not a retrigger (see tracker_process_effects_tick0()).
inline void tracker_trigger_note(const SongHeader *song, const Event &ev,
                                  PlayerChannelState &pcs, ChannelTick &ct, bool linear) {
    if (ev.instrument != 0) pcs.instrument = ev.instrument;

    if (ev.note == 97) {  // key off
        pcs.vol64 = 0;
        pcs.volL = 0;
        pcs.volR = 0;
        ct.flags |= TICK_KEY_OFF;
        return;
    }
    if (ev.note < 1 || ev.note > 96) return;

    TrackerSampleResolution res = tracker_resolve_note_sample(song, pcs.instrument, ev.note);
    if (!res.ok) return;
    const SampleHeader &sh = *res.sh;

    const uint8_t *base = tracker_blob_base(song);
    const uint32_t *incs = reinterpret_cast<const uint32_t *>(base + sh.note_increments_offset);
    pcs.inc = incs[ev.note - 1];
    pcs.sample_id = res.global_sample;
    pcs.base_note = res.base_note;
    pcs.finetune = (double)sh.finetune;
    pcs.period = tracker_note_to_period(pcs.base_note, pcs.finetune, linear);
    pcs.vibrato_pos = 0;  // new note resets vibrato phase (no E4 "keep position" support yet)

    uint32_t vol64 = sh.default_volume;
    if ((VolEffect)ev.vol_effect == VolEffect::SET_VOLUME) vol64 = ev.vol_param;
    if (vol64 > 64) vol64 = 64;
    pcs.vol64 = vol64;

    int16_t pan_q15 = tracker_xm_pan_to_q15((uint32_t)sh.default_panning);
    if ((VolEffect)ev.vol_effect == VolEffect::SET_PANNING) {
        // Vol-column panning is a 4-bit field (0..15): scale to the full pan range.
        uint32_t p4 = ev.vol_param & 0xF;
        pan_q15 = (int16_t)(int32_t)((p4 * 65535u / 15u) - 32768u);
    }
    pcs.pan_q15 = pan_q15;
    tracker_recompute_volume(pcs);

    pcs.trigger++;
    ct.flags |= TICK_NOTE_ON;
    ct.start_pos = 0;
}

// Tick-0-only processing for one channel: note trigger (or tone-portamento
// retarget, which suppresses the trigger), effect-memory resolution, and
// every effect that acts instantaneously rather than per-tick (Cxx, Bxx,
// Dxx, Fxx). Sets `pcs.active_effect`/`active_param` for
// tracker_apply_pitch_vol_effect() to consume on ticks 1..speed-1 of this
// row -- resolved here (memory substituted) so later ticks never re-touch
// the memory slots. An event with no effect column entry correctly resets
// active_effect to NONE, which is what makes a continuous effect require
// restating every row it runs on (tracker.md/#19: "tick-0-vs-later-tick
// semantics ... is where the real work is").
inline void tracker_process_effects_tick0(const SongHeader *song, PlayerState &st, uint32_t c,
                                           const Event &ev, ChannelTick &ct, bool linear) {
    PlayerChannelState &pcs = st.ch[c];
    Effect eff = (Effect)ev.effect;
    uint8_t param = ev.effect_param;

    pcs.active_effect = Effect::NONE;
    pcs.active_param = 0;

    if (eff == Effect::TONE_PORTA) {
        // A note here retargets the glide instead of retriggering the
        // sample -- the whole point of tone portamento. The instrument
        // column, key-off, and vol column still behave normally.
        if (ev.instrument != 0) pcs.instrument = ev.instrument;
        if (ev.note == 97) {
            pcs.vol64 = 0;
            pcs.volL = 0;
            pcs.volR = 0;
            ct.flags |= TICK_KEY_OFF;
        } else if (ev.note >= 1 && ev.note <= 96) {
            TrackerSampleResolution res = tracker_resolve_note_sample(song, pcs.instrument, ev.note);
            if (res.ok) pcs.tone_porta_target = tracker_note_to_period(res.base_note, (double)res.sh->finetune, linear);
        }
        tracker_apply_vol_column(ev, pcs);
        if (param != 0) pcs.tone_porta_memory = param;
        pcs.active_effect = Effect::TONE_PORTA;
        pcs.active_param = pcs.tone_porta_memory;
        return;
    }

    tracker_trigger_note(song, ev, pcs, ct, linear);

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
            tracker_recompute_volume(pcs);
            break;
        }
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
        default:
            break;
    }
}

// Ticks 1..speed-1 of a row: advances/applies whichever continuous effect
// tracker_process_effects_tick0() resolved for this channel, and returns
// this tick's increment. Porta up/down and tone portamento write the new
// pitch back into `pcs.period`/`pcs.inc` -- a lasting change that must
// survive into rows that don't restate the effect. Arpeggio and vibrato are
// transient: they compute an offset from `pcs.period` for this tick only
// and never touch `pcs.inc`, so the channel returns to its unmodulated pitch
// the instant the effect isn't restated.
inline uint32_t tracker_apply_pitch_vol_effect(PlayerChannelState &pcs, uint32_t tick_in_row, bool linear) {
    switch (pcs.active_effect) {
        case Effect::ARPEGGIO: {
            uint32_t which = tick_in_row % 3;
            if (which == 0) return pcs.inc;
            // Verified against openmpt123: tick 1 of the cycle is the *low*
            // nibble's offset, tick 2 the high nibble's -- base, +y, +x, not
            // the other way round.
            int offset = (which == 1) ? (pcs.active_param & 0x0F) : (pcs.active_param >> 4);
            double p = tracker_note_to_period(pcs.base_note + offset, pcs.finetune, linear);
            return tracker_period_to_inc(p, linear);
        }
        // No *4 here: some published FT2 pseudocode multiplies the raw
        // param by 4 for linear-frequency slides, but that's compensating
        // for a period scale (64 units/semitone) this engine doesn't use --
        // periods.py's linear period is already 16 units/semitone (see
        // TRACKER_LINEAR_BASE_PERIOD's comment), so applying that *4 here
        // on top would double the compensation. Verified against
        // openmpt123: pitch-tracking a held porta-up run showed this
        // engine's frequency climbing ~4x faster per tick than the
        // reference's until the multiplier was removed.
        case Effect::PORTA_UP:
            pcs.period -= (double)pcs.active_param;
            tracker_clamp_period(pcs.period);
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            return pcs.inc;
        case Effect::PORTA_DOWN:
            pcs.period += (double)pcs.active_param;
            tracker_clamp_period(pcs.period);
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            return pcs.inc;
        case Effect::TONE_PORTA: {
            double step = (double)pcs.active_param;
            if (pcs.period < pcs.tone_porta_target) {
                pcs.period += step;
                if (pcs.period > pcs.tone_porta_target) pcs.period = pcs.tone_porta_target;
            } else if (pcs.period > pcs.tone_porta_target) {
                pcs.period -= step;
                if (pcs.period < pcs.tone_porta_target) pcs.period = pcs.tone_porta_target;
            }
            pcs.inc = tracker_period_to_inc(pcs.period, linear);
            return pcs.inc;
        }
        case Effect::VIBRATO: {
            // Empirically calibrated against openmpt123 (pitch-tracked a
            // long held vibrato run): this table's "quarter cycle" (index 0
            // to its peak at 16) took roughly 22 ticks at speed 1, closer to
            // a >>1 position-to-index shift than the >>2 some published FT2
            // pseudocode uses. Not chased to bit-exactness -- see this
            // function's header comment on why continuous pitch effects
            // aren't held to that bar.
            uint8_t idx = (pcs.vibrato_pos >> 1) & 0x1F;
            int32_t amplitude = (int32_t)TRACKER_VIBRATO_SINE[idx];
            int32_t delta = (amplitude * (int32_t)pcs.vibrato_depth) >> 5;
            if (pcs.vibrato_pos >= 128) delta = -delta;
            pcs.vibrato_pos = (uint8_t)(pcs.vibrato_pos + pcs.vibrato_speed);
            return tracker_period_to_inc(pcs.period + (double)delta, linear);
        }
        case Effect::VOLUME_SLIDE: {
            uint8_t hi = pcs.active_param >> 4, lo = pcs.active_param & 0x0F;
            int32_t v = (int32_t)pcs.vol64;
            if (hi != 0) v += hi;
            else v -= lo;
            if (v < 0) v = 0;
            if (v > 64) v = 64;
            pcs.vol64 = (uint32_t)v;
            tracker_recompute_volume(pcs);
            return pcs.inc;
        }
        default:
            return pcs.inc;
    }
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

    bool row_boundary = (st.tick_in_row == 0);
    for (uint32_t c = 0; c < TRACKER_MAX_CHANNELS; c++) {
        ChannelTick &ct = out.ch[c];
        ct.flags = 0;
        ct.start_pos = 0;
        if (c >= num_channels) continue;

        PlayerChannelState &pcs = st.ch[c];
        if (row_boundary) {
            const Event &ev = tracker_event_at(song, pat, st.row, c, num_channels);
            tracker_process_effects_tick0(song, st, c, ev, ct, linear);
            ct.inc = pcs.inc;
        } else {
            ct.inc = tracker_apply_pitch_vol_effect(pcs, st.tick_in_row, linear);
        }
        ct.tgt_volL = pcs.volL;
        ct.tgt_volR = pcs.volR;
        ct.sample_id = pcs.sample_id;
        ct.trigger = pcs.trigger;
    }

    // Fxx (tick 0, above) must be visible in the block it belongs to --
    // tracker.md: "Fxx tempo changes have to take effect at the tick
    // boundary they belong to" -- so this reads st.samples_per_tick after
    // the per-channel loop, not before it.
    out.samples_per_tick = st.samples_per_tick;

    bool looped = false;
    st.tick_in_row++;
    if (st.tick_in_row >= st.speed) {
        st.tick_in_row = 0;
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
    return looped;
}

// Builds a small SRAM-resident TrackerSample descriptor per song sample,
// read once at song-load time -- the only place that ever dereferences
// SampleHeader (flash-resident on device). tracker.md's rationale for
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
            sh.length, sh.loop_start, sh.loop_end, sh.loop_type != 0,
        };
    }
}

// Consumes one produced TickBlock into the mixer's per-channel voice state
// -- tracker.md's render-loop pseudocode calls this apply_tick(). Pure
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
        } else if (v.active) {
            v.inc = tracker_latch_inc(ct.inc);
        }
        v.tgt_volL = ct.tgt_volL;
        v.tgt_volR = ct.tgt_volR;
    }
}
