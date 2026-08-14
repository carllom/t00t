#pragma once

#include "player.h"

// Core 0's tracker player (#18): song load (flash -> SRAM), the ordered
// TickBlock ring, and play/stop/seek transport. player_tick() (via
// player_produce_tick(), player.h) runs here rather than on Core 1 because
// it's a pure function of song state and needs nothing Core 1 owns, and
// because it keeps pattern-data flash reads off Core 1's SRAM-only working
// set (module_tracker.md "Core Split") -- see tracker_apply_tick()'s doc comment
// in player.h for why that boundary matters for the resident sample table
// below too.

// Cap on distinct samples a song can reference -- sized generously against
// XM's realistic range, not a hard format limit. Bounds-checked against the
// loaded song's num_samples at init.
static constexpr uint32_t TRACKER_MAX_RESIDENT_SAMPLES = 256;

// Extern'd for Core 1 (audio_engine.cpp). Neither is touched by Core 1
// before tracker_player_task_init() has run -- main.cpp calls it before
// multicore_launch_core1() -- so there's no race on first use.
extern TickRing g_tracker_tick_ring;
extern TrackerSample g_tracker_resident_samples[TRACKER_MAX_RESIDENT_SAMPLES];
extern uint32_t g_tracker_num_channels;  // song->num_channels, cached once at load

// Loads the embedded demo song (tracker_song_blob.h) into SRAM, builds the
// resident sample table, calls osc_init_sine() (player_produce_tick()'s
// note-trigger path needs its wavetable now that it runs on Core 0, not
// Core 1), and primes the ring before returning. Call once, before
// multicore_launch_core1().
void tracker_player_task_init();

// Drains the reverse multicore FIFO (Core 1's non-blocking "tick consumed"
// doorbell -- values are unused, it's purely a wake signal) and, if
// playing, tops the ring back up to full. Call once per Core 0 main-loop
// iteration. output.cpp's DMA IRQ already wakes that loop every
// SAMPLES_PER_BUFFER/SAMPLE_RATE seconds regardless of MIDI activity, so no
// extra timer is needed to keep this responsive.
void tracker_player_task();

void tracker_transport_play();                    // resume producing from the current position
void tracker_transport_stop();                    // stop producing; ring drains to silence
void tracker_transport_seek(uint32_t order_idx);  // rewind to an order, clamped to [0, num_orders)

// Read-only playback snapshot for the display (#24). Taken from the row/tick
// that produced the most recently *pushed* TickBlock, not live PlayerState --
// module_tracker.md "Display": Core 0 already holds this, no reverse channel from
// Core 1 needed, and it runs one tick ahead of what's audible (invisible at
// 20ms). `active_mask` bit c is set when channel c was actually audible on
// that tick -- nonzero pitch increment *and* nonzero post-pan target volume
// (see fill_ring()'s comment in player_task.cpp for why inc alone isn't a
// silence signal: it holds the last-triggered note's pitch and doesn't go
// back to 0 on key-off/fadeout/envelope release the way volume does).
struct TrackerUiState {
    uint32_t order_idx;
    uint32_t pattern_idx;
    uint32_t row;
    uint32_t active_mask;
};
void tracker_player_ui_state(TrackerUiState *out);

// The loaded song's header (title, tracker name, channel count, ...) --
// read-only, valid once tracker_player_task_init() has run.
const SongHeader *tracker_player_song();
