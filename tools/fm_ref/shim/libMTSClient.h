#pragma once

// No-op replacement for ODDSound MTS-ESP's client header.
//
// Dexed uses MTS-ESP to let a host retune notes live. `Dx7Note` consults it in
// exactly two places (dx7note.cc): `MTS_HasMaster()` gates the whole microtuning
// branch, and `MTS_NoteToFrequency()` supplies the retuned frequency inside it.
// With `MTS_HasMaster()` returning false the branch is never taken and
// `Dx7Note::osc_freq()` falls through to the standard `tuning_state_` path --
// which is the only path the F0 reference rig wants (see shim/tuning.h).
//
// `MTS_NoteToFrequency` still has to link, so it returns plain 12-TET rather
// than something that would be silently wrong if the branch were ever taken.

#include <cmath>

struct MTSClient;

inline bool MTS_HasMaster(MTSClient *) { return false; }

inline double MTS_NoteToFrequency(MTSClient *, char midinote, char /*channel*/) {
    return 440.0 * std::pow(2.0, (midinote - 69) / 12.0);
}

inline MTSClient *MTS_RegisterClient() { return nullptr; }
inline void MTS_DeregisterClient(MTSClient *) {}
