#pragma once

// Standard-tuning-only replacement for Dexed's Source/msfa/tuning.h.
//
// The real one pulls in JUCE, the Surge `Tunings` library and libMTSClient to
// support Scala/KBM microtuning -- none of which the DX7 had, and none of which
// the F0 reference rig compares against. `Dx7Note` only ever calls
// `midinote_to_logfreq()` and `is_standard_tuning()` (verified by grep over
// dx7note.cc), so this provides exactly those.
//
// Log-frequency format, taken from the real tuning.cc's standard-tuning path:
//
//     logfreq = logScaledFrequencyForMidiNote(n) * (1 << 24) + 50857777
//
// i.e. Q24 log2(frequency), where the `50857777` base is log2 of MIDI note 0
// (8.175798915 Hz) in the same Q24 units: 50857777 / 2^24 = 3.03136, and
// 2^3.03136 = 8.1758 Hz. Under 12-TET `logScaledFrequencyForMidiNote(n)` is
// just `n / 12.0`, so the whole thing collapses to the expression below.
// Sanity check, A4: 69/12 * 2^24 + 50857777 = 147326769; 2^(147326769/2^24)
// = 440.000 Hz.

#include "synth.h"

#include <cstdint>
#include <memory>
#include <string>

class TuningState {
public:
    virtual ~TuningState() {}

    virtual int32_t midinote_to_logfreq(int midinote) {
        const int32_t base = 50857777;  // Q24 log2(8.175798915 Hz) == MIDI note 0
        const int32_t step = 1 << 24;
        return (int32_t)((midinote / 12.0) * step) + base;
    }

    virtual bool is_standard_tuning() { return true; }
    virtual int scale_length() { return 12; }
    virtual std::string display_tuning_str() { return "Standard Tuning"; }
};

inline std::shared_ptr<TuningState> createStandardTuning() {
    return std::make_shared<TuningState>();
}
