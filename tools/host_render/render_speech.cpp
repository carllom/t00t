// Host-side proof of src/engines/speech/render.h (#27): renders the speech
// engine skeleton's test tone -- native 22.05 kHz, zero-order-held x2 to
// 44.1 kHz -- to WAV using the exact same speech_render_test_tone() the
// device's audio_engine.cpp calls from its Core 1 render loop. There is no
// openmpt123 equivalent for a formant synth (speech.md "Testing"), so this
// harness *is* the reference from here on: same source, host compiler, WAV
// out, diffable against a device capture to separate DSP bugs from embedded
// bugs. No pico-sdk, no ARM intrinsics -- render.h only touches osc/sine.h
// and pan.h (see its own header comment for why that matters).
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_speech
#include "../../src/engines/speech/render.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

// Mirrors src/engines/speech/engine.h's SPEECH_RATE (SAMPLE_RATE/2) without
// pulling engine.h/engine_base.h in here — see render.h's comment on why.
static constexpr uint32_t SPEECH_RATE = SAMPLE_RATE / 2;
static constexpr float    TEST_TONE_HZ = 220.0f;
static constexpr uint32_t NATIVE_BUFFER = 128;  // matches the device's default SAMPLES_PER_BUFFER(256)/2

int main() {
    osc_init_sine();  // speech_render_test_tone()'s wavetable source

    const uint32_t inc = (uint32_t)((TEST_TONE_HZ / (float)SPEECH_RATE)
        * (float)WAVETABLE_SIZE * (float)(1 << PHASE_FRAC_BITS));
    uint32_t phase = 0;

    const uint32_t total_native_frames = SPEECH_RATE * 2;      // 2 seconds of native audio
    const uint32_t total_output_frames = total_native_frames * 2;  // ZOH x2
    std::vector<int16_t> out(total_output_frames * 2);         // interleaved stereo

    std::vector<int32_t> dry_l(NATIVE_BUFFER * 2), dry_r(NATIVE_BUFFER * 2);

    uint32_t done_native = 0, done_out = 0;
    int32_t peak = 0;
    while (done_native < total_native_frames) {
        uint32_t n = std::min(NATIVE_BUFFER, total_native_frames - done_native);
        speech_render_test_tone(phase, inc, /*pan=*/0, dry_l.data(), dry_r.data(), n);
        for (uint32_t i = 0; i < n * 2; i++) {
            int16_t l = (int16_t)dry_l[i];
            int16_t r = (int16_t)dry_r[i];
            out[(done_out + i) * 2 + 0] = l;
            out[(done_out + i) * 2 + 1] = r;
            peak = std::max({ peak, (int32_t)(l < 0 ? -l : l), (int32_t)(r < 0 ? -r : r) });
        }
        done_native += n;
        done_out += n * 2;
    }

    bool ok = write_wav_pcm16("speech_test_tone.wav", out, SAMPLE_RATE, 2);
    // Frame-count arithmetic sanity: ZOH x2 means every consecutive pair of
    // output frames is exactly identical -- this is the property every later
    // formant slice sits on (issue #27's acceptance criterion).
    bool zoh_ok = true;
    for (uint32_t i = 0; i + 1 < total_output_frames && zoh_ok; i += 2) {
        if (out[i * 2] != out[(i + 1) * 2] || out[i * 2 + 1] != out[(i + 1) * 2 + 1]) zoh_ok = false;
    }

    printf("%s: wrote speech_test_tone.wav -- %u output frames (%u native @ %u Hz, ZOH x2 -> %u Hz), peak=%d\n",
           ok && zoh_ok ? "PASS" : "FAIL", total_output_frames, total_native_frames, SPEECH_RATE, SAMPLE_RATE, peak);
    if (!zoh_ok) printf("  FAIL: a ZOH x2 pair diverged -- frame-count arithmetic is broken\n");
    if (!ok) printf("  FAIL: could not write WAV file\n");

    return (ok && zoh_ok) ? 0 : 1;
}
