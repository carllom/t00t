#pragma once

#include "platform.h"
#include "midi/midi_parser.h"
#include <cstdint>

// The t00t SysEx protocol, the editor's way into a module on the patch
// platform (docs/platform.md). Runs on Core 0, into the same Edit buffers
// NRPN and CC edit.
//
// Every message is F0 7D 54 <command> <body> F7: 0x7D is the MIDI
// non-commercial manufacturer ID, 0x54 ('T') the t00t sub-ID. Commands:
//
//   01 identify request     (none)
//   02 identify reply       module ID, schema version, protocol version,
//                           Part count, parameter count (2 x 7 bits, LSB first)
//   10 parameter change     Part, parameter ID (2 x 7 bits, LSB first),
//                           value (int21)
//   20 edit-buffer request  Part
//   21 edit-buffer dump     Part, the stored Patch (stored_format.h), packed
//
// Reserved ranges: 30-3F store and banks, 40-4F descriptor dump, 60-7F
// Data Exchange transfers. Unknown commands and malformed messages are
// ignored, as is anything for another manufacturer or sub-ID; a dump of a
// Patch for another module is rejected (patch_decode()).
//
// Encodings, since SysEx data bytes carry 7 bits:
//   int21   two's complement over 3 bytes of 7 bits, least significant first
//   packed  8-bit data in groups of up to 7 bytes, each group led by a byte
//           holding their high bits (bit i = byte i of the group), followed
//           by the bytes with their high bit cleared

static constexpr uint8_t kSysExManufacturer = 0x7D;
static constexpr uint8_t kSysExSubId = 0x54;
static constexpr uint8_t kSysExProtocolVersion = 1;

enum class SysExCommand : uint8_t {
    IDENTIFY_REQUEST    = 0x01,
    IDENTIFY_REPLY      = 0x02,
    PARAM_CHANGE        = 0x10,
    EDIT_BUFFER_REQUEST = 0x20,
    EDIT_BUFFER_DUMP    = 0x21,
};

// Writes F0 7D 54 <command> to `out`; returns the 4 bytes written.
inline uint32_t sysex_begin(uint8_t *out, SysExCommand command) {
    out[0] = 0xF0;
    out[1] = kSysExManufacturer;
    out[2] = kSysExSubId;
    out[3] = (uint8_t)command;
    return 4;
}

// Sends one complete message, F0 to F7.
using SysExSend = void (*)(const uint8_t *msg, uint32_t len);

constexpr uint32_t sysex_packed_len(uint32_t n) { return n + (n + 6) / 7; }

inline uint32_t sysex_pack(const uint8_t *in, uint32_t n, uint8_t *out) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 7) {
        uint32_t group = (n - i < 7) ? n - i : 7;
        uint8_t high = 0;
        for (uint32_t j = 0; j < group; j++) high |= (uint8_t)(((in[i + j] >> 7) & 1u) << j);
        out[o++] = high;
        for (uint32_t j = 0; j < group; j++) out[o++] = in[i + j] & 0x7F;
    }
    return o;
}

// Unpacks into `out` (capacity `cap`). Returns the byte count, or 0 if the
// input is malformed (a high-bits byte with no data after it) or too long.
inline uint32_t sysex_unpack(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 8) {
        uint32_t group = (n - i < 8) ? n - i - 1 : 7;
        if (group == 0 || o + group > cap) return 0;
        uint8_t high = in[i];
        for (uint32_t j = 0; j < group; j++) {
            out[o++] = (uint8_t)(in[i + 1 + j] | (((high >> j) & 1u) << 7));
        }
    }
    return o;
}

inline int32_t sysex_int21(const uint8_t *p) {
    int32_t v = (int32_t)(p[0] | (p[1] << 7) | (p[2] << 14));
    return (v & 0x100000) ? v - 0x200000 : v;
}

// Handle one SysEx message (`msg`: the bytes between F0 and F7). Replies go
// through `send`.
template <typename Module>
inline void patch_sysex_handle(PatchPlatformT<Module> &platform, const uint8_t *msg, uint32_t len,
                               SysExSend send) {
    using Platform = PatchPlatformT<Module>;
    // A dump is 3 header bytes, the Part and the packed Patch; it has to fit
    // the parser's SysEx buffer or the device would discard its own dumps.
    static_assert(4 + sysex_packed_len(patch_stored_size(Module::kDescriptors)) <= MIDI_SYSEX_MAX,
                  "this module's edit-buffer dump is longer than MIDI_SYSEX_MAX");
    if (len < 3 || msg[0] != kSysExManufacturer || msg[1] != kSysExSubId) return;
    const SysExCommand command = (SysExCommand)msg[2];
    const uint8_t *body = msg + 3;
    const uint32_t body_len = len - 3;

    switch (command) {
        case SysExCommand::IDENTIFY_REQUEST: {
            uint8_t reply[11];
            uint32_t n = sysex_begin(reply, SysExCommand::IDENTIFY_REPLY);
            reply[n++] = (uint8_t)Module::kModuleId;
            reply[n++] = Module::kSchemaVersion;
            reply[n++] = kSysExProtocolVersion;
            reply[n++] = (uint8_t)Platform::kPartCount;
            reply[n++] = (uint8_t)(Platform::kParamCount & 0x7F);
            reply[n++] = (uint8_t)(Platform::kParamCount >> 7);
            reply[n++] = 0xF7;
            send(reply, n);
            return;
        }
        case SysExCommand::PARAM_CHANGE: {
            if (body_len != 6) return;
            platform.set_param(body[0], (uint32_t)(body[1] | (body[2] << 7)), sysex_int21(body + 3));
            return;
        }
        case SysExCommand::EDIT_BUFFER_REQUEST: {
            if (body_len != 1 || body[0] >= Platform::kPartCount) return;
            constexpr uint32_t kStored = patch_stored_size(Module::kDescriptors);
            uint8_t stored[kStored];
            uint8_t reply[5 + sysex_packed_len(kStored) + 1];
            platform.encode_edit_buffer(body[0], stored, kStored);
            uint32_t n = sysex_begin(reply, SysExCommand::EDIT_BUFFER_DUMP);
            reply[n++] = body[0];
            n += sysex_pack(stored, kStored, reply + n);
            reply[n++] = 0xF7;
            send(reply, n);
            return;
        }
        case SysExCommand::EDIT_BUFFER_DUMP: {
            if (body_len < 2) return;
            uint8_t stored[MIDI_SYSEX_MAX];  // unpacking only shrinks a message
            uint32_t n = sysex_unpack(body + 1, body_len - 1, stored, sizeof stored);
            if (n == 0) return;
            platform.load(body[0], StoredPatchRef{ stored, n });
            return;
        }
        default:
            return;
    }
}
