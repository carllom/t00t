#pragma once

// Minimal stand-in for a real engine's src/engines/<name>/engine.h, providing
// only what src/voice_alloc.h actually needs (MAX_VOICES) so it can be
// host-built without pulling in a real engine's full VoiceParams/
// engine_base.h chain -- which drags in "hardware/sync.h" (pico-sdk) and
// isn't available on the host. This directory must be listed ahead of
// src/ in the test target's include path so this shadows any real engine.h.

#include <cstdint>

static constexpr uint32_t MAX_VOICES = 16;
