#pragma once

#include <cstdint>

// MIDI event types emitted by the parser
enum MidiMsgType : uint8_t {
    MIDI_NOTE_OFF = 0,
    MIDI_NOTE_ON  = 1,
    MIDI_CC       = 2,  // control change
    MIDI_PITCH_BEND = 3,
    MIDI_PROGRAM_CHANGE = 4,
    // System real-time (single byte, no channel/data) — for the sequencer clock.
    MIDI_CLOCK    = 5,  // 0xF8, 24 per quarter note
    MIDI_START    = 6,  // 0xFA
    MIDI_CONTINUE = 7,  // 0xFB
    MIDI_STOP     = 8,  // 0xFC
    // System Exclusive, F0 ... F7, assembled: the bytes between F0 and F7
    // are in MidiParser::sysex()/sysex_len() until the next feed().
    MIDI_SYSEX    = 9,
};

// Generic 2-data-byte event. Field meaning depends on type:
//   NOTE_ON/OFF      : data1 = note,       data2 = velocity
//   CC               : data1 = controller, data2 = value
//   PITCH_BEND       : data1 = LSB,        data2 = MSB (14-bit = (data2<<7)|data1)
//   PROGRAM_CHANGE   : data1 = program,    data2 = 0
//   SYSEX            : unused -- see MidiParser::sysex()
struct MidiEvent {
    MidiMsgType type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
};

// Longest SysEx message kept, counting the bytes between F0 and F7; a longer
// one is discarded whole.
static constexpr uint32_t MIDI_SYSEX_MAX = 320;

// Transport-agnostic MIDI byte-stream parser.
// Handles running status and system real-time pass-through.
// Feeds from USB MIDI stream or UART — same byte format.
// Emits note on/off, CC, pitch bend, program change and SysEx events.
struct MidiParser {
    uint8_t status;    // current running status byte
    uint8_t data[2];   // data byte accumulator
    uint8_t count;     // data bytes received so far

    bool in_sysex;       // between F0 and its end
    bool sysex_overflow; // the current SysEx outgrew sysex_buf, so it's dropped
    uint32_t sysex_count;
    uint8_t sysex_buf[MIDI_SYSEX_MAX];

    void init() {
        status = 0;
        count = 0;
        in_sysex = false;
        sysex_overflow = false;
        sysex_count = 0;
    }

    // The SysEx message a MIDI_SYSEX event reported, without F0/F7.
    const uint8_t *sysex() const { return sysex_buf; }
    uint32_t sysex_len() const { return sysex_count; }

    // Number of data bytes expected for a channel voice status byte
    static uint8_t expected_data(uint8_t s) {
        uint8_t hi = s & 0xF0;
        // Program Change (0xC0), Channel Pressure (0xD0): 1 data byte
        // All others (Note Off/On, Poly AT, CC, Pitch Bend): 2 data bytes
        return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    }

    // Feed one byte. Returns true if a note on/off event was completed.
    bool feed(uint8_t byte, MidiEvent &out) {
        // System real-time (0xF8-0xFF): single-byte, may arrive mid-message.
        // Emit the transport messages; leave running status untouched.
        if (byte >= 0xF8) {
            switch (byte) {
                case 0xF8: out = { MIDI_CLOCK,    0, 0, 0 }; return true;
                case 0xFA: out = { MIDI_START,    0, 0, 0 }; return true;
                case 0xFB: out = { MIDI_CONTINUE, 0, 0, 0 }; return true;
                case 0xFC: out = { MIDI_STOP,     0, 0, 0 }; return true;
                default:   return false;   // active sensing (0xFE), reset (0xFF)
            }
        }

        // SysEx: F0 starts one, F7 completes it. Any other status byte
        // abandons an unfinished one and is then handled as usual.
        if (byte == 0xF0) {
            in_sysex = true;
            sysex_overflow = false;
            sysex_count = 0;
            status = 0;
            count = 0;
            return false;
        }
        if (byte == 0xF7) {
            bool complete = in_sysex && !sysex_overflow;
            in_sysex = false;
            status = 0;
            count = 0;
            if (!complete) return false;
            out = { MIDI_SYSEX, 0, 0, 0 };
            return true;
        }
        if (byte & 0x80) in_sysex = false;
        if (in_sysex) {
            if (sysex_count < MIDI_SYSEX_MAX) sysex_buf[sysex_count++] = byte;
            else sysex_overflow = true;
            return false;
        }

        // Other system common (0xF1-0xF6): cancel running status
        if (byte >= 0xF0) {
            status = 0;
            count = 0;
            return false;
        }

        // Status byte (0x80-0xEF): set running status
        if (byte & 0x80) {
            status = byte;
            count = 0;
            return false;
        }

        // Data byte — need a valid channel voice status
        if (status == 0) return false;

        data[count++] = byte;
        if (count < expected_data(status)) return false;

        // Complete message — reset count for running status
        count = 0;

        uint8_t hi = status & 0xF0;
        uint8_t ch = status & 0x0F;
        switch (hi) {
            case 0x90:
                if (data[1] > 0) {
                    out = { MIDI_NOTE_ON, ch, data[0], data[1] };
                } else {
                    out = { MIDI_NOTE_OFF, ch, data[0], data[1] };  // note-on vel 0
                }
                return true;
            case 0x80:
                out = { MIDI_NOTE_OFF, ch, data[0], data[1] };
                return true;
            case 0xB0:
                out = { MIDI_CC, ch, data[0], data[1] };
                return true;
            case 0xE0:
                out = { MIDI_PITCH_BEND, ch, data[0], data[1] };
                return true;
            case 0xC0:
                out = { MIDI_PROGRAM_CHANGE, ch, data[0], 0 };
                return true;
            default:
                return false;  // poly AT (0xA0), channel pressure (0xD0) — ignored
        }
    }
};

// Where bytes come from. Each source has its own parser, so a message (a
// SysEx above all) split across reads isn't corrupted by bytes from the
// other transport arriving in between.
enum MidiSource : uint8_t { MIDI_SOURCE_USB, MIDI_SOURCE_UART, MIDI_SOURCE_COUNT };

// One parser per source, fed through whichever the transport selected.
struct MidiParsers {
    MidiParser by_source[MIDI_SOURCE_COUNT];
    uint8_t active = 0;

    void init() {
        for (MidiParser &p : by_source) p.init();
        active = 0;
    }
    // Called by a transport before it hands its bytes on.
    void select(MidiSource source) { active = source; }

    bool feed(uint8_t byte, MidiEvent &out) { return by_source[active].feed(byte, out); }
    const uint8_t *sysex() const { return by_source[active].sysex(); }
    uint32_t sysex_len() const { return by_source[active].sysex_len(); }
};

// Shared instance -- the set of per-source parsers: only one engine is ever
// linked at a time, and MIDI_USB and MIDI_UART both feed their bytes through
// midi_controller_process() after select(), so no module needs (or gets to
// keep) a private copy of its own.
inline MidiParsers midi_parser;
