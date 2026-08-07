#pragma once

#include <atomic>
#include <cstdint>

#include "audio_common.h"
#include "blob_format.h"
#include "mixer.h"
#include "pan.h"

// Core 0 tracker player (#17, pulled forward from #18's "notes only, no
// effects" scope — tracker.md build order step 4) — order-list/pattern walk,
// note triggering, the TickBlock/TickRing shapes from the design doc. Pure
// integer math over a `SongHeader*` blob, no pico-sdk: this header is
// included by both tools/host_render/render_xm_device.cpp (the #17 reference-
// diff harness) and, once #18 wires the hardware side (multicore ring
// atomics/barriers, flash->SRAM sample loading, transport), the real Core 0
// engine. `player_produce_tick()` is a pure function of `PlayerState` plus
// the blob -- identical behaviour on host or device is the whole point
// (tracker.md: "Any divergence between the host and device render paths
// defeats the purpose").
//
// Effect column is entirely unused this issue -- every command in it (`0`-
// `Z`) is #19's job. The volume column's SET_VOLUME/SET_PANNING are honoured
// because they aren't per-tick effects: they set the note's own velocity/pan
// at trigger time, exactly like the note and instrument columns already do,
// not a ramp or slide applied over the row. Key-off (note 97) cuts the
// voice's volume target to 0 immediately rather than releasing an envelope --
// there is no envelope yet (#20); this is the pre-envelope approximation.
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
    TICK_NOTE_CUT = 0x04,  // reserved for #19's Cxx; never set by this player
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
// volume (restated into every produced ChannelTick) plus the "current
// instrument" FT2 remembers across notes that omit the instrument column.
struct PlayerChannelState {
    uint32_t inc = 0;
    int32_t volL = 0, volR = 0;
    uint32_t sample_id = 0;
    uint8_t instrument = 0;  // 1-based; 0 = none yet
    uint8_t trigger = 0;
};

struct PlayerState {
    uint32_t order_idx = 0;
    uint32_t row = 0;
    uint32_t tick_in_row = 0;
    uint32_t speed = 6;              // ticks per row
    uint32_t samples_per_tick = 0;
    PlayerChannelState ch[TRACKER_MAX_CHANNELS];
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

// Resolves one channel's note trigger (tick 0 of a row only) against the
// instrument/sample tables and updates `pcs` in place. No-op if there's no
// note, no instrument on record for this channel, or the instrument's
// sample_map has no sample mapped to this note (0xFF) -- all silently
// leave the channel exactly as it was, matching FT2's "nothing to play"
// behaviour rather than cutting a note that just has no data behind it.
inline void tracker_trigger_note(const SongHeader *song, const Event &ev,
                                  PlayerChannelState &pcs, ChannelTick &ct) {
    if (ev.instrument != 0) pcs.instrument = ev.instrument;

    if (ev.note == 97) {  // key off
        pcs.volL = 0;
        pcs.volR = 0;
        ct.flags |= TICK_KEY_OFF;
        return;
    }
    if (ev.note < 1 || ev.note > 96) return;
    if (pcs.instrument == 0 || pcs.instrument > song->num_instruments) return;

    const InstrumentHeader &inst = tracker_instrument_table(song)[pcs.instrument - 1];
    if (inst.sample_map_offset == 0) return;

    const uint8_t *base = tracker_blob_base(song);
    uint8_t local = base[inst.sample_map_offset + (ev.note - 1)];
    if (local == 0xFF || local >= inst.num_samples) return;

    const uint32_t *sample_index = reinterpret_cast<const uint32_t *>(base + inst.sample_index_offset);
    uint32_t global_sample = sample_index[local];
    if (global_sample >= song->num_samples) return;
    const SampleHeader &sh = tracker_sample_table(song)[global_sample];

    const uint32_t *incs = reinterpret_cast<const uint32_t *>(base + sh.note_increments_offset);
    pcs.inc = incs[ev.note - 1];
    pcs.sample_id = global_sample;

    uint32_t vol64 = sh.default_volume;
    if ((VolEffect)ev.vol_effect == VolEffect::SET_VOLUME) vol64 = ev.vol_param;
    if (vol64 > 64) vol64 = 64;
    int32_t vol_q15 = (int32_t)((vol64 * 32767u + 32u) / 64u);

    int16_t pan_q15 = tracker_xm_pan_to_q15((uint32_t)sh.default_panning);
    if ((VolEffect)ev.vol_effect == VolEffect::SET_PANNING) {
        // Vol-column panning is a 4-bit field (0..15): scale to the full pan range.
        uint32_t p4 = ev.vol_param & 0xF;
        pan_q15 = (int16_t)(int32_t)((p4 * 65535u / 15u) - 32768u);
    }
    int32_t gain_l, gain_r;
    pan_gains_q15(pan_q15, gain_l, gain_r);
    pcs.volL = (vol_q15 * gain_l) >> 15;
    pcs.volR = (vol_q15 * gain_r) >> 15;

    pcs.trigger++;
    ct.flags |= TICK_NOTE_ON;
    ct.start_pos = 0;
}

// Advances the player by exactly one tick and fills `out` with that tick's
// per-channel state. Pure function of `st` + the blob -- no I/O, no
// allocation -- so a host harness and (once #18 wires the hardware side)
// Core 0 can call it identically. Returns true on the tick where the order
// list wraps back to the restart position, i.e. "one full pass just
// completed" -- device playback ignores this and loops forever; a host
// renderer uses it as its natural stop condition.
inline bool player_produce_tick(PlayerState &st, const SongHeader *song, TickBlock &out) {
    out.samples_per_tick = st.samples_per_tick;

    const uint32_t num_channels = song->num_channels;
    const uint8_t *orders = tracker_order_table(song);
    uint32_t pat_idx = orders[st.order_idx];
    const PatternHeader &pat = tracker_pattern_table(song)[pat_idx];

    bool row_boundary = (st.tick_in_row == 0);
    for (uint32_t c = 0; c < TRACKER_MAX_CHANNELS; c++) {
        ChannelTick &ct = out.ch[c];
        ct.flags = 0;
        ct.start_pos = 0;
        if (row_boundary && c < num_channels) {
            const Event &ev = tracker_event_at(song, pat, st.row, c, num_channels);
            tracker_trigger_note(song, ev, st.ch[c], ct);
        }
        ct.inc = st.ch[c].inc;
        ct.tgt_volL = st.ch[c].volL;
        ct.tgt_volR = st.ch[c].volR;
        ct.sample_id = st.ch[c].sample_id;
        ct.trigger = st.ch[c].trigger;
    }

    bool looped = false;
    st.tick_in_row++;
    if (st.tick_in_row >= st.speed) {
        st.tick_in_row = 0;
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
        }
        v.tgt_volL = ct.tgt_volL;
        v.tgt_volR = ct.tgt_volR;
    }
}
