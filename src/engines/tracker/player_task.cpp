#include "player_task.h"
#include "tracker_song_blob.h"
#include "osc/sine.h"
#include "pico/multicore.h"
#include <cstring>

// Sample data must live in SRAM, never XIP (tracker.md "Memory Strategy"):
// 32 voices reading scattered, non-integer-stride addresses would thrash
// the 8 KB XIP cache and evict Core 0's code with it. 380 KB matches
// tools/xm2t00t/blob_writer.py's DEFAULT_SRAM_BUDGET_BYTES -- the host
// converter already refuses to build a blob whose sample_data_bytes
// exceeds that budget, so any blob that converted successfully fits here.
static constexpr uint32_t TRACKER_SRAM_BUDGET_BYTES = 380u * 1024u;
static int8_t s_sample_sram[TRACKER_SRAM_BUDGET_BYTES];

TickRing g_tracker_tick_ring;
TrackerSample g_tracker_resident_samples[TRACKER_MAX_RESIDENT_SAMPLES];
uint32_t g_tracker_num_channels = 0;

static const SongHeader *s_song;
static PlayerState s_player_state;
static bool s_playing = false;

// Display snapshot (#24), latched alongside each produced tick -- see
// TrackerUiState's comment (player_task.h) for why this is captured *before*
// player_produce_tick() rather than read back from PlayerState afterwards
// (its row/order_idx advance logic runs at the tail of that call, so reading
// them post-call would show the next row one tick early at every row
// boundary rather than one tick late throughout, matching tracker.md's "one
// tick ahead" framing instead of jumping the gun on the row itself).
static TrackerUiState s_ui_state;

// Tops the ring back up to full from the current PlayerState. Called both
// at init (priming) and from tracker_player_task() (steady state).
static void fill_ring() {
    while (!g_tracker_tick_ring.full()) {
        s_ui_state.order_idx = s_player_state.order_idx;
        s_ui_state.row = s_player_state.row;
        s_ui_state.pattern_idx = tracker_order_table(s_song)[s_ui_state.order_idx];

        TickBlock &tb = g_tracker_tick_ring.write_slot();
        player_produce_tick(s_player_state, s_song, tb);

        // "Active" means audible, not "has a pitch": ct.inc only reflects
        // the last-triggered note's pitch and, once a channel has ever been
        // triggered, essentially never returns to 0 on its own (tracker.md/
        // player.h: pcs.inc is pitch state, not a sounding flag) -- so an
        // inc-only test lit every channel that had ever played a note and
        // never went dark again. tgt_volL/tgt_volR (Q15, post-pan) is the
        // per-tick output of tracker_resolve_envelope_volpan(), which *does*
        // reach exactly 0 on key-off cut, fadeout completion, volume-envelope
        // decay with no sustain, and SET_VOLUME/volslide-to-0 -- the actual
        // "gone quiet" cases a real module hits mid-song.
        uint32_t mask = 0;
        for (uint32_t c = 0; c < g_tracker_num_channels; c++) {
            const ChannelTick &ct = tb.ch[c];
            if (ct.inc != 0 && (ct.tgt_volL != 0 || ct.tgt_volR != 0)) mask |= (1u << c);
        }
        s_ui_state.active_mask = mask;

        g_tracker_tick_ring.push();
    }
}

void tracker_player_task_init() {
    // player_produce_tick()'s note-trigger path (player.h:
    // tracker_trigger_note()) calls pan_gains_q15(), which reads this
    // wavetable -- and that now runs here, on Core 0, not on Core 1 like
    // the old test rig. Must happen before the first fill_ring() below.
    osc_init_sine();

    s_song = reinterpret_cast<const SongHeader *>(tracker_song_blob_data);

    // Defensive: halt rather than silently overrun SRAM or the resident
    // sample table if a hand-swapped-in module doesn't respect the budgets
    // the host converter is supposed to enforce.
    if (s_song->sample_data_bytes > TRACKER_SRAM_BUDGET_BYTES || s_song->num_samples > TRACKER_MAX_RESIDENT_SAMPLES) {
        while (true) {}
    }

    const uint8_t *blob_base = tracker_blob_base(s_song);
    memcpy(s_sample_sram, blob_base + s_song->sample_data_offset, s_song->sample_data_bytes);

    tracker_build_resident_samples(s_song, s_sample_sram, s_song->sample_data_offset, g_tracker_resident_samples);
    g_tracker_num_channels = s_song->num_channels;

    player_init(s_player_state, s_song);
    s_playing = true;  // auto-play: buttonless, MIDI-only board -- should make sound with nothing attached
    fill_ring();
}

void tracker_player_task() {
    uint32_t val;
    while (multicore_fifo_pop_timeout_us(0, &val)) {}  // drain Core 1's doorbell acks
    if (s_playing) fill_ring();
}

void tracker_transport_play() { s_playing = true; }
void tracker_transport_stop() { s_playing = false; }

void tracker_transport_seek(uint32_t order_idx) {
    player_init(s_player_state, s_song);
    if (order_idx >= s_song->num_orders) order_idx = s_song->num_orders - 1;
    s_player_state.order_idx = order_idx;
}

void tracker_player_ui_state(TrackerUiState *out) { *out = s_ui_state; }

const SongHeader *tracker_player_song() { return s_song; }
