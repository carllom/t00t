// Host-side proof of src/engines/tracker/player.h (#17) plus the #17
// reference-diff harness itself: two modes in one binary.
//
//   render_xm_device                       -- run built-in unit tests of
//                                              player.h against an in-memory
//                                              synthetic blob (no files, no
//                                              external tools needed).
//   render_xm_device render <blob.bin> <out.wav> <out_trace.csv> [max_s]
//                                           -- load a real converted module
//                                              (tools/xm2t00t/xm2t00t.py
//                                              convert --raw-blob) and drive
//                                              the real player.h + mixer.h
//                                              tick-by-tick, exactly as
//                                              tracker.md's render loop
//                                              pseudocode describes: produce
//                                              one TickBlock, apply it to the
//                                              voices, render exactly that
//                                              tick's samples_per_tick frames
//                                              through tracker_render_buffer().
//                                              Writes a WAV plus a per-tick
//                                              CSV trace (frame, order,
//                                              pattern, row, tick, triggers)
//                                              that tools/host_render/diff_xm.py
//                                              uses to localize a divergence
//                                              to a row/tick/channel instead
//                                              of just a sample offset.
//
// Run via `python3 tools/host_render/diff_xm.py`, which builds this target,
// generates the render, and does the actual openmpt123 diff -- see that file
// for the tolerance policy. This binary only ever produces the *device-path*
// WAV; it has no opinion about what counts as a match.
#include "engines/tracker/player.h"
#include "engines/tracker/mixer.h"
#include "osc/sine.h"
#include "wav_writer.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// In-memory synthetic blob builder (test mode only) -- mirrors
// tools/xm2t00t/blob_writer.py's reserve/write_at/append pattern, in C++,
// against the same struct layout blob_format.h declares. All blob structs
// are 4-byte-aligned uint32_t/int32_t/char[] fields (verified by the
// static_asserts in blob_format.h), so a struct can be memcpy'd straight in.
// ============================================================================
namespace test_blob {

struct Builder {
    std::vector<uint8_t> buf;
    uint32_t tell() const { return (uint32_t)buf.size(); }
    template <typename T>
    uint32_t append(const T &v) {
        uint32_t start = tell();
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
        buf.insert(buf.end(), p, p + sizeof(T));
        return start;
    }
    uint32_t append_bytes(const void *p, size_t n) {
        uint32_t start = tell();
        const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
        buf.insert(buf.end(), b, b + n);
        return start;
    }
};

static void set_name(char *dst, size_t n, const char *src) {
    std::memset(dst, 0, n);
    std::strncpy(dst, src, n - 1);
}

// Two channels, one pattern (4 rows), one instrument mapping every note to
// the same one-shot 8-sample instrument. Row 0 ch0 triggers a note; row 1
// ch1 triggers a different note at half volume (vol column SET_VOLUME); row
// 2 ch0 retriggers (generation counter must bump); row 3 ch0 gets a key-off.
// Speed (ticks/row) = 3, so each row produces 3 ticks (tick 0 = the
// trigger, ticks 1-2 = plain restatement, flags == 0).
static std::vector<uint8_t> build() {
    Builder b;
    uint32_t song_off = b.tell();
    b.buf.resize(b.buf.size() + sizeof(SongHeader));  // reserved, backfilled below

    uint32_t order_off = b.append_bytes("\x00", 1);  // one order -> pattern 0

    uint32_t pattern_table_off = b.tell();
    b.buf.resize(b.buf.size() + sizeof(PatternHeader));

    Event events[4 * 2] = {};
    events[0 * 2 + 0] = {49, 1, 0, 0, 0, 0};   // row0 ch0: C-4, instrument 1
    events[1 * 2 + 1] = {61, 1, (uint8_t)VolEffect::SET_VOLUME, 32, 0, 0};  // row1 ch1: half volume
    events[2 * 2 + 0] = {49, 1, 0, 0, 0, 0};   // row2 ch0: retrigger
    events[3 * 2 + 0] = {97, 0, 0, 0, 0, 0};   // row3 ch0: key off
    uint32_t event_off = b.append_bytes(events, sizeof(events));

    PatternHeader pat{4, event_off};
    std::memcpy(b.buf.data() + pattern_table_off, &pat, sizeof(pat));

    uint32_t instrument_table_off = b.tell();
    b.buf.resize(b.buf.size() + sizeof(InstrumentHeader));

    uint8_t sample_map[96];
    std::memset(sample_map, 0, sizeof(sample_map));  // every note -> local sample 0
    uint32_t sample_map_off = b.append_bytes(sample_map, sizeof(sample_map));
    uint32_t sample_idx = 0;
    uint32_t sample_index_off = b.append(sample_idx);

    InstrumentHeader inst{};
    inst.sample_map_offset = sample_map_off;
    inst.num_samples = 1;
    inst.sample_index_offset = sample_index_off;
    set_name(inst.name, sizeof(inst.name), "test");
    std::memcpy(b.buf.data() + instrument_table_off, &inst, sizeof(inst));

    uint32_t sample_table_off = b.tell();
    b.buf.resize(b.buf.size() + sizeof(SampleHeader));

    // Distinct, arbitrary Q8.24 increments per note -- lets a test assert
    // the exact table entry was latched, not just "some nonzero value".
    uint32_t incs[T00T_NOTES_PER_TABLE];
    for (uint32_t i = 0; i < T00T_NOTES_PER_TABLE; i++) incs[i] = (i + 1) * 1000u;
    uint32_t incs_off = b.append_bytes(incs, sizeof(incs));

    int8_t pcm[9] = {1, 2, 3, 4, 5, 6, 7, 8, 8};  // 8 frames + guard (one-shot: last value)
    uint32_t data_off = b.append_bytes(pcm, sizeof(pcm));

    SampleHeader sh{};
    sh.length = 8;
    sh.loop_start = 0;
    sh.loop_end = 8;
    sh.loop_type = 0;
    sh.finetune = 0;
    sh.relative_note = 0;
    sh.default_volume = 64;
    sh.default_panning = 128;
    sh.data_offset = data_off;
    sh.note_increments_offset = incs_off;
    set_name(sh.name, sizeof(sh.name), "test");
    std::memcpy(b.buf.data() + sample_table_off, &sh, sizeof(sh));

    SongHeader hdr{};
    std::memcpy(hdr.magic, T00T_BLOB_MAGIC, 4);
    hdr.version = T00T_BLOB_VERSION;
    hdr.num_channels = 2;
    hdr.freq_table = 1;
    hdr.num_orders = 1;
    hdr.num_patterns = 1;
    hdr.num_instruments = 1;
    hdr.num_samples = 1;
    hdr.default_tempo = 3;  // ticks per row
    hdr.default_bpm = 125;
    hdr.restart_order = 0;
    hdr.order_table_offset = order_off;
    hdr.pattern_table_offset = pattern_table_off;
    hdr.instrument_table_offset = instrument_table_off;
    hdr.sample_table_offset = sample_table_off;
    hdr.sample_data_offset = data_off;
    hdr.sample_data_bytes = sizeof(pcm);
    hdr.total_size = b.tell();
    set_name(hdr.name, sizeof(hdr.name), "player_test");
    std::memcpy(b.buf.data() + song_off, &hdr, sizeof(hdr));

    return b.buf;
}

}  // namespace test_blob

// ============================================================================
// Unit tests
// ============================================================================

static bool test_note_trigger_and_retrigger() {
    std::vector<uint8_t> blob = test_blob::build();
    const SongHeader *song = reinterpret_cast<const SongHeader *>(blob.data());

    PlayerState st;
    player_init(st, song);
    bool ok = true;

    // Expected samples_per_tick at 125 BPM: 44100*2.5/125 = 882 -- also the
    // exact figure tracker.md's own worked timing example uses.
    if (st.samples_per_tick != 882) {
        printf("  FAIL: samples_per_tick=%u, want 882\n", st.samples_per_tick);
        ok = false;
    }

    // Row 0, tick 0: ch0 triggers note 49 (table index 48), ch1 silent.
    TickBlock tb, tb2;
    bool looped = player_produce_tick(st, song, tb);
    if (looped) { printf("  FAIL: looped on the very first tick\n"); ok = false; }
    if (!(tb.ch[0].flags & TICK_NOTE_ON)) { printf("  FAIL: ch0 row0 tick0 did not trigger\n"); ok = false; }
    if (tb.ch[0].inc != 49u * 1000u) {
        printf("  FAIL: ch0 inc=%u, want %u (note 49's table entry)\n", tb.ch[0].inc, 49u * 1000u);
        ok = false;
    }
    if (tb.ch[0].trigger != 1) { printf("  FAIL: ch0 trigger=%u, want 1\n", tb.ch[0].trigger); ok = false; }
    if (tb.ch[0].tgt_volL <= 0 || tb.ch[0].tgt_volR <= 0) { printf("  FAIL: ch0 triggered at zero volume\n"); ok = false; }
    if (tb.ch[1].flags != 0) { printf("  FAIL: ch1 row0 tick0 unexpectedly triggered\n"); ok = false; }
    int32_t full_vol_l = tb.ch[0].tgt_volL;

    // Row 0, ticks 1-2: restated, not retriggered.
    for (int t = 0; t < 2; t++) {
        player_produce_tick(st, song, tb);
        if (tb.ch[0].flags != 0) { printf("  FAIL: ch0 row0 tick%d unexpectedly retriggered\n", t + 1); ok = false; }
        if (tb.ch[0].inc != 49u * 1000u || tb.ch[0].trigger != 1) {
            printf("  FAIL: ch0 row0 tick%d did not restate prior state\n", t + 1);
            ok = false;
        }
    }

    // Row 1, tick 0: ch1 triggers note 61 at half volume (vol column 32/64);
    // ch0 restates row 0's note (no event this row).
    looped |= player_produce_tick(st, song, tb);
    for (int i = 0; i < 2; i++) looped |= player_produce_tick(st, song, tb2);  // ticks 1-2, discarded
    if (!(tb.ch[1].flags & TICK_NOTE_ON)) { printf("  FAIL: ch1 row1 tick0 did not trigger\n"); ok = false; }
    if (tb.ch[1].inc != 61u * 1000u) {
        printf("  FAIL: ch1 inc=%u, want %u (note 61's table entry)\n", tb.ch[1].inc, 61u * 1000u);
        ok = false;
    }
    double ratio = (double)tb.ch[1].tgt_volL / (double)full_vol_l;
    if (ratio < 0.45 || ratio > 0.55) {
        printf("  FAIL: half-volume trigger gave ratio %.3f, want ~0.5\n", ratio);
        ok = false;
    }
    if (tb.ch[0].flags != 0 || tb.ch[0].inc != 49u * 1000u) {
        printf("  FAIL: ch0 row1 did not carry its note forward unchanged\n");
        ok = false;
    }

    // Row 2, tick 0: ch0 retriggers -- generation counter must bump.
    looped |= player_produce_tick(st, song, tb);
    for (int i = 0; i < 2; i++) looped |= player_produce_tick(st, song, tb2);  // ticks 1-2, discarded
    if (!(tb.ch[0].flags & TICK_NOTE_ON)) { printf("  FAIL: ch0 row2 tick0 did not retrigger\n"); ok = false; }
    if (tb.ch[0].trigger != 2) { printf("  FAIL: ch0 retrigger generation=%u, want 2\n", tb.ch[0].trigger); ok = false; }

    // Row 3, tick 0: ch0 key-off -- volume target cut to 0, no note-on flag.
    looped |= player_produce_tick(st, song, tb);
    for (int i = 0; i < 2; i++) looped |= player_produce_tick(st, song, tb2);  // ticks 1-2, discarded
    if (!(tb.ch[0].flags & TICK_KEY_OFF)) { printf("  FAIL: ch0 row3 tick0 missing TICK_KEY_OFF\n"); ok = false; }
    if (tb.ch[0].flags & TICK_NOTE_ON) { printf("  FAIL: key-off incorrectly also set TICK_NOTE_ON\n"); ok = false; }
    if (tb.ch[0].tgt_volL != 0 || tb.ch[0].tgt_volR != 0) { printf("  FAIL: key-off did not cut volume target to 0\n"); ok = false; }

    // This 1-order, 4-row, speed-3 song is 12 ticks long; the 12th call
    // (row3's last tick) completes the only order and must report looped.
    if (!looped) { printf("  FAIL: single-order song never reported looped==true\n"); ok = false; }

    printf("  %s: trigger/retrigger/vol-column/key-off all matched expectations\n", ok ? "PASS" : "FAIL");
    return ok;
}

static bool test_missing_sample_map_entry_is_silent() {
    std::vector<uint8_t> blob = test_blob::build();
    // Point every sample-map entry at 0xFF ("no sample") instead of 0.
    SongHeader *song = reinterpret_cast<SongHeader *>(blob.data());
    const InstrumentHeader &inst = tracker_instrument_table(song)[0];
    std::memset(blob.data() + inst.sample_map_offset, 0xFF, 96);

    PlayerState st;
    player_init(st, song);
    TickBlock tb;
    player_produce_tick(st, song, tb);

    bool ok = (tb.ch[0].flags == 0) && (tb.ch[0].inc == 0);
    printf("  %s: a note with no sample mapped to it produces no trigger\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ============================================================================
// Render mode: load a real converted blob, drive player.h + mixer.h
// tick-by-tick, write WAV + CSV trace.
// ============================================================================

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static bool cmd_render(const std::string &blob_path, const std::string &wav_path,
                        const std::string &trace_path, double max_seconds) {
    std::vector<uint8_t> blob = read_file(blob_path);
    if (blob.size() < sizeof(SongHeader)) {
        fprintf(stderr, "render_xm_device: %s: too small to be a t00t blob\n", blob_path.c_str());
        return false;
    }
    const SongHeader *song = reinterpret_cast<const SongHeader *>(blob.data());
    if (std::memcmp(song->magic, T00T_BLOB_MAGIC, 4) != 0) {
        fprintf(stderr, "render_xm_device: %s: bad magic\n", blob_path.c_str());
        return false;
    }
    if (blob.size() < song->total_size) {
        fprintf(stderr, "render_xm_device: %s: truncated (%zu bytes, header says %u)\n",
                blob_path.c_str(), blob.size(), song->total_size);
        return false;
    }

    FILE *trace = fopen(trace_path.c_str(), "w");
    if (!trace) {
        fprintf(stderr, "render_xm_device: cannot open %s for writing\n", trace_path.c_str());
        return false;
    }
    fprintf(trace, "frame,order,pattern,row,tick,triggers\n");

    osc_init_sine();

    const uint32_t num_channels = song->num_channels;
    std::vector<TrackerVoice> voices(num_channels);
    std::vector<TrackerSample> chan_sample(num_channels);
    for (auto &v : voices) v = TrackerVoice{};

    PlayerState st;
    player_init(st, song);

    const uint32_t max_frames = (uint32_t)(max_seconds * SAMPLE_RATE);
    std::vector<int16_t> out;
    out.reserve(max_frames * 2);

    const uint8_t *base = tracker_blob_base(song);
    const SampleHeader *samples = tracker_sample_table(song);
    const uint8_t *orders = tracker_order_table(song);

    bool looped = false;
    uint32_t total_frames = 0;
    while (!looped && total_frames < max_frames) {
        uint32_t order = st.order_idx, row = st.row, tick_in_row = st.tick_in_row;
        uint32_t pat_idx = orders[order];
        const PatternHeader &pat = tracker_pattern_table(song)[pat_idx];

        TickBlock tb;
        looped = player_produce_tick(st, song, tb);

        std::string triggers;
        for (uint32_t c = 0; c < num_channels; c++) {
            const ChannelTick &ct = tb.ch[c];
            TrackerVoice &v = voices[c];
            if (ct.flags & TICK_NOTE_ON) {
                const SampleHeader &sh = samples[ct.sample_id];
                chan_sample[c] = TrackerSample{
                    reinterpret_cast<const int8_t *>(base + sh.data_offset),
                    sh.length, sh.loop_start, sh.loop_end, sh.loop_type != 0,
                };
                v.sample = &chan_sample[c];
                v.pos = 0;
                v.inc = tracker_latch_inc(ct.inc);
                v.active = true;

                const Event &ev = tracker_event_at(song, pat, row, c, num_channels);
                char entry[64];
                snprintf(entry, sizeof(entry), "%sc%u:n%u:i%u:s%u", triggers.empty() ? "" : ";",
                         c, ev.note, ev.instrument, ct.sample_id);
                triggers += entry;
            } else if (ct.flags & TICK_KEY_OFF) {
                char entry[32];
                snprintf(entry, sizeof(entry), "%sc%u:off", triggers.empty() ? "" : ";", c);
                triggers += entry;
            }
            v.tgt_volL = ct.tgt_volL;
            v.tgt_volR = ct.tgt_volR;
        }

        fprintf(trace, "%u,%u,%u,%u,%u,%s\n", total_frames, order, pat_idx, row, tick_in_row, triggers.c_str());

        uint32_t n = tb.samples_per_tick;
        size_t old_size = out.size();
        out.resize(old_size + (size_t)n * 2);
        TrackerTickState tick_state{n, n};
        tracker_render_buffer(voices.data(), num_channels, out.data() + old_size, n, tick_state);
        total_frames += n;
    }
    fclose(trace);

    if (!looped) {
        fprintf(stderr, "render_xm_device: %s: hit the %.0fs safety cap before the order list "
                        "looped -- either a very long module or a stuck player\n",
                blob_path.c_str(), max_seconds);
    }

    write_wav_pcm16(wav_path, out, SAMPLE_RATE, 2);
    printf("Wrote %s (%.2fs, %u frames) and %s\n", wav_path.c_str(),
           (double)total_frames / SAMPLE_RATE, total_frames, trace_path.c_str());
    return true;
}

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "render") {
        if (argc < 5) {
            fprintf(stderr, "usage: render_xm_device render <blob.bin> <out.wav> <out_trace.csv> [max_seconds]\n");
            return 2;
        }
        double max_seconds = argc >= 6 ? atof(argv[5]) : 300.0;
        return cmd_render(argv[2], argv[3], argv[4], max_seconds) ? 0 : 1;
    }

    osc_init_sine();  // pan_gains_q15's quadrature source (mixer_test/audio_engine convention)

    bool ok = true;
    printf("== note trigger, restatement, retrigger, vol-column, key-off ==\n");
    ok = test_note_trigger_and_retrigger() && ok;
    printf("\n== sample_map 0xFF entry produces no trigger ==\n");
    ok = test_missing_sample_map_entry_is_silent() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
