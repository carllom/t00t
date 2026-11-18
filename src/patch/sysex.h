#pragma once

#include "common.h"
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
//   30 store request        Part, user slot (2 x 7 bits): store the Part's
//                           Edit buffer into that user-bank slot
//   31 store reply          Part, user slot (2 x 7 bits), result
//                           (StoreResult, user_bank.h)
//   40 descriptor dump request  (none); answered by, in order:
//   41   table begin        table, descriptor count (2 x 7 bits)
//   42   descriptor         table, ID (2 x 7 bits), min, max, default (int21
//                           each), unit, curve, class, flags (bit 0: quick
//                           edit), default CC (7F = none), name, group
//   43   labels             table, ID (2 x 7 bits), label count, labels --
//                           after its descriptor, for ENUM parameters with labels
//   44   dump end           (none)
//                           Table 0 is the module's, then table 1 the common
//                           components' (common.h). One message per
//                           descriptor, sent as the outgoing queue has room.
//
// Reserved ranges: 32-3F store and banks, 45-4F more descriptor messages,
// 60-7F Data Exchange transfers. Unknown commands and malformed messages are
// ignored, as is anything for another manufacturer or sub-ID; a dump of a
// Patch for another module is rejected (patch_decode()).
//
// Encodings, since SysEx data bytes carry 7 bits:
//   int21   two's complement over 3 bytes of 7 bits, least significant first
//   packed  8-bit data in groups of up to 7 bytes, each group led by a byte
//           holding their high bits (bit i = byte i of the group), followed
//           by the bytes with their high bit cleared
//   string  a length byte, then 7-bit ASCII

static constexpr uint8_t kSysExManufacturer = 0x7D;
static constexpr uint8_t kSysExSubId = 0x54;
static constexpr uint8_t kSysExProtocolVersion = 1;

enum class SysExCommand : uint8_t {
    IDENTIFY_REQUEST    = 0x01,
    IDENTIFY_REPLY      = 0x02,
    PARAM_CHANGE        = 0x10,
    EDIT_BUFFER_REQUEST = 0x20,
    EDIT_BUFFER_DUMP    = 0x21,
    STORE_REQUEST       = 0x30,
    STORE_REPLY         = 0x31,
    DESCRIPTOR_DUMP_REQUEST = 0x40,
    DESCRIPTOR_TABLE_BEGIN  = 0x41,
    DESCRIPTOR              = 0x42,
    DESCRIPTOR_LABELS       = 0x43,
    DESCRIPTOR_DUMP_END     = 0x44,
};

enum class DescriptorTable : uint8_t { MODULE = 0, COMMON = 1 };

// Writes F0 7D 54 <command> to `out`; returns the 4 bytes written.
inline uint32_t sysex_begin(uint8_t *out, SysExCommand command) {
    out[0] = 0xF0;
    out[1] = kSysExManufacturer;
    out[2] = kSysExSubId;
    out[3] = (uint8_t)command;
    return 4;
}

// Sends one complete message, F0 to F7. False if there's no room for it
// right now (it wasn't sent; try again later).
using SysExSend = bool (*)(const uint8_t *msg, uint32_t len);

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

inline uint32_t sysex_put_int21(uint8_t *out, int32_t value) {
    uint32_t v = (uint32_t)value & 0x1FFFFFu;
    out[0] = (uint8_t)(v & 0x7F);
    out[1] = (uint8_t)((v >> 7) & 0x7F);
    out[2] = (uint8_t)((v >> 14) & 0x7F);
    return 3;
}

// IDs and counts: 2 x 7 bits, least significant first.
inline uint32_t sysex_put_id(uint8_t *out, uint32_t id) {
    out[0] = (uint8_t)(id & 0x7F);
    out[1] = (uint8_t)((id >> 7) & 0x7F);
    return 2;
}
inline uint32_t sysex_id(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 7)); }

constexpr uint32_t sysex_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

inline uint32_t sysex_put_string(uint8_t *out, const char *s) {
    uint32_t n = sysex_strlen(s);
    out[0] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) out[1 + i] = (uint8_t)(s[i] & 0x7F);
    return 1 + n;
}

// Message sizes, F0 to F7, for the descriptor dump -- field by field as
// PatchSysExT::send_dump_step() writes them.
constexpr uint32_t kSysExHeaderLen = 4;  // F0 7D 54 <command>
constexpr uint32_t sysex_string_len(const char *s) { return 1 + sysex_strlen(s); }
constexpr uint32_t sysex_descriptor_len(const ParamDescriptor &d) {
    return kSysExHeaderLen + 1 /* table */ + 2 /* id */ + 3 * 3 /* min, max, default */
         + 5 /* unit, curve, class, flags, default CC */
         + sysex_string_len(d.name) + sysex_string_len(d.group) + 1 /* F7 */;
}
constexpr uint32_t sysex_labels_len(const ParamDescriptor &d) {
    uint32_t n = kSysExHeaderLen + 1 /* table */ + 2 /* id */ + 1 /* count */ + 1 /* F7 */;
    for (uint32_t i = 0; i < d.label_count; i++) n += sysex_string_len(d.labels[i]);
    return n;
}

// A string the dump can carry: 7-bit ASCII, at most 127 characters.
constexpr bool sysex_string_ok(const char *s) {
    uint32_t n = 0;
    for (; s[n]; n++) {
        if ((uint8_t)s[n] >= 0x80) return false;
    }
    return n <= 0x7F;
}

constexpr bool sysex_int21_ok(int32_t v) { return v >= -0x100000 && v < 0x100000; }

// Every descriptor of `table` can be dumped: its strings and values fit
// their encodings, and each message fits a receiver's MIDI_SYSEX_MAX buffer.
template <uint32_t N>
constexpr bool sysex_descriptor_dump_fits(const ParamDescriptor (&table)[N]) {
    for (uint32_t i = 0; i < N; i++) {
        const ParamDescriptor &d = table[i];
        if (!sysex_string_ok(d.name) || !sysex_string_ok(d.group)) return false;
        if (!sysex_int21_ok(d.min) || !sysex_int21_ok(d.max) || !sysex_int21_ok(d.def)) return false;
        if (sysex_descriptor_len(d) > MIDI_SYSEX_MAX + 2) return false;
        if (!d.labels) continue;
        if (d.label_count > 0x7F || sysex_labels_len(d) > MIDI_SYSEX_MAX + 2) return false;
        for (uint32_t l = 0; l < d.label_count; l++) {
            if (!sysex_string_ok(d.labels[l])) return false;
        }
    }
    return true;
}

// The SysEx side of a module on the patch platform: handles incoming
// messages and paces the descriptor dump. Core 0 only.
template <typename Module>
class PatchSysExT {
public:
    using Platform = PatchPlatformT<Module>;

    PatchSysExT(Platform &platform, SysExSend send) : platform_(platform), send_(send) {}

    // Handle one SysEx message (`msg`: the bytes between F0 and F7).
    void handle(const uint8_t *msg, uint32_t len);

    // Core 0, once per main-loop pass: send a reply the outgoing queue
    // refused earlier, then continue a descriptor dump.
    void task() {
        if (held_len_ && send_(held_, held_len_)) held_len_ = 0;
        if (!held_len_) pump_dump();
    }

private:
    // Send a reply now, or hold it for task() if the queue is full; a newer
    // reply replaces a held one.
    void reply(const uint8_t *msg, uint32_t len) {
        if (!held_len_ && send_(msg, len)) return;
        for (uint32_t i = 0; i < len; i++) held_[i] = msg[i];
        held_len_ = len;
    }

    // The longest reply: an edit-buffer dump.
    static constexpr uint32_t kMaxReply = 5 + sysex_packed_len(patch_stored_size(Module::kDescriptors)) + 1;

    // Descriptor dump progress: which table, which descriptor, and which of
    // its messages comes next.
    enum class DumpStep : uint8_t { IDLE, TABLE_BEGIN, DESCRIPTOR, LABELS, END };

    void pump_dump();
    bool send_dump_step();

    Platform &platform_;
    SysExSend send_;
    uint8_t held_[kMaxReply];
    uint32_t held_len_ = 0;
    DumpStep step_ = DumpStep::IDLE;
    DescriptorTable table_ = DescriptorTable::MODULE;
    uint32_t index_ = 0;
};

// Handle one SysEx message (`msg`: the bytes between F0 and F7). Replies go
// through `send`.
template <typename Module>
inline void PatchSysExT<Module>::handle(const uint8_t *msg, uint32_t len) {
    static_assert(sysex_descriptor_dump_fits(Module::kDescriptors) &&
                  sysex_descriptor_dump_fits(kCommonDescriptors),
                  "a descriptor dump message is longer than MIDI_SYSEX_MAX");
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
            uint8_t msg_out[11];
            uint32_t n = sysex_begin(msg_out, SysExCommand::IDENTIFY_REPLY);
            msg_out[n++] = (uint8_t)Module::kModuleId;
            msg_out[n++] = Module::kSchemaVersion;
            msg_out[n++] = kSysExProtocolVersion;
            msg_out[n++] = (uint8_t)Platform::kPartCount;
            n += sysex_put_id(msg_out + n, Platform::kParamCount);
            msg_out[n++] = 0xF7;
            reply(msg_out, n);
            return;
        }
        case SysExCommand::PARAM_CHANGE: {
            if (body_len != 6) return;
            platform_.set_param(body[0], sysex_id(body + 1), sysex_int21(body + 3));
            return;
        }
        case SysExCommand::EDIT_BUFFER_REQUEST: {
            if (body_len != 1 || body[0] >= Platform::kPartCount) return;
            constexpr uint32_t kStored = patch_stored_size(Module::kDescriptors);
            uint8_t stored[kStored];
            uint8_t msg_out[5 + sysex_packed_len(kStored) + 1];
            platform_.encode_edit_buffer(body[0], stored, kStored);
            uint32_t n = sysex_begin(msg_out, SysExCommand::EDIT_BUFFER_DUMP);
            msg_out[n++] = body[0];
            n += sysex_pack(stored, kStored, msg_out + n);
            msg_out[n++] = 0xF7;
            reply(msg_out, n);
            return;
        }
        case SysExCommand::EDIT_BUFFER_DUMP: {
            if (body_len < 2) return;
            uint8_t stored[MIDI_SYSEX_MAX];  // unpacking only shrinks a message
            uint32_t n = sysex_unpack(body + 1, body_len - 1, stored, sizeof stored);
            if (n == 0) return;
            platform_.load(body[0], StoredPatchRef{ stored, n });
            return;
        }
        case SysExCommand::STORE_REQUEST: {
            if (body_len != 3 || body[0] >= Platform::kPartCount) return;
            const uint32_t slot = sysex_id(body + 1);
            const StoreResult result = platform_.store(body[0], slot);
            uint8_t msg_out[9];
            uint32_t n = sysex_begin(msg_out, SysExCommand::STORE_REPLY);
            msg_out[n++] = body[0];
            n += sysex_put_id(msg_out + n, slot);
            msg_out[n++] = (uint8_t)result;
            msg_out[n++] = 0xF7;
            reply(msg_out, n);
            return;
        }
        case SysExCommand::DESCRIPTOR_DUMP_REQUEST: {
            if (body_len != 0) return;
            step_ = DumpStep::TABLE_BEGIN;
            table_ = DescriptorTable::MODULE;
            index_ = 0;
            pump_dump();
            return;
        }
        default:
            return;
    }
}

template <typename Module>
inline void PatchSysExT<Module>::pump_dump() {
    while (step_ != DumpStep::IDLE && send_dump_step()) {}
}

// Send the dump's next message and advance; false if it didn't fit.
template <typename Module>
inline bool PatchSysExT<Module>::send_dump_step() {
    const bool module = (table_ == DescriptorTable::MODULE);
    const ParamDescriptor *table = module ? Module::kDescriptors : kCommonDescriptors;
    const uint32_t count = module ? Platform::kParamCount : COMMON_P_COUNT;
    uint8_t msg[MIDI_SYSEX_MAX + 2];
    uint32_t n = 0;

    switch (step_) {
        case DumpStep::TABLE_BEGIN:
            n = sysex_begin(msg, SysExCommand::DESCRIPTOR_TABLE_BEGIN);
            msg[n++] = (uint8_t)table_;
            n += sysex_put_id(msg + n, count);
            break;
        case DumpStep::DESCRIPTOR: {
            const ParamDescriptor &d = table[index_];
            n = sysex_begin(msg, SysExCommand::DESCRIPTOR);
            msg[n++] = (uint8_t)table_;
            n += sysex_put_id(msg + n, d.id);
            n += sysex_put_int21(msg + n, d.min);
            n += sysex_put_int21(msg + n, d.max);
            n += sysex_put_int21(msg + n, d.def);
            msg[n++] = (uint8_t)d.unit;
            msg[n++] = (uint8_t)d.curve;
            msg[n++] = (uint8_t)d.cls;
            msg[n++] = d.quick_edit ? 1 : 0;
            msg[n++] = d.default_cc == kNoDefaultCc ? 0x7F : d.default_cc;
            n += sysex_put_string(msg + n, d.name);
            n += sysex_put_string(msg + n, d.group);
            break;
        }
        case DumpStep::LABELS: {
            const ParamDescriptor &d = table[index_];
            n = sysex_begin(msg, SysExCommand::DESCRIPTOR_LABELS);
            msg[n++] = (uint8_t)table_;
            n += sysex_put_id(msg + n, d.id);
            msg[n++] = d.label_count;
            for (uint32_t i = 0; i < d.label_count; i++) n += sysex_put_string(msg + n, d.labels[i]);
            break;
        }
        case DumpStep::END:
            n = sysex_begin(msg, SysExCommand::DESCRIPTOR_DUMP_END);
            break;
        case DumpStep::IDLE:
            return false;
    }
    msg[n++] = 0xF7;
    if (!send_(msg, n)) return false;

    // Advance: TABLE_BEGIN -> DESCRIPTOR (-> LABELS) per descriptor -> the
    // next table -> END -> IDLE.
    switch (step_) {
        case DumpStep::DESCRIPTOR:
            if (table[index_].labels) { step_ = DumpStep::LABELS; return true; }
            [[fallthrough]];
        case DumpStep::LABELS:
            index_++;
            [[fallthrough]];
        case DumpStep::TABLE_BEGIN:
            if (index_ < count) { step_ = DumpStep::DESCRIPTOR; return true; }
            if (table_ == DescriptorTable::MODULE) {
                table_ = DescriptorTable::COMMON;
                index_ = 0;
                step_ = DumpStep::TABLE_BEGIN;
            } else {
                step_ = DumpStep::END;
            }
            return true;
        case DumpStep::END:
        case DumpStep::IDLE:
            step_ = DumpStep::IDLE;
            return true;
    }
    return true;
}
