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
};

// Generic 2-data-byte event. Field meaning depends on type:
//   NOTE_ON/OFF      : data1 = note,       data2 = velocity
//   CC               : data1 = controller, data2 = value
//   PITCH_BEND       : data1 = LSB,        data2 = MSB (14-bit = (data2<<7)|data1)
//   PROGRAM_CHANGE   : data1 = program,    data2 = 0
struct MidiEvent {
    MidiMsgType type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
};

// Transport-agnostic MIDI byte-stream parser.
// Handles running status and system real-time pass-through.
// Feeds from USB MIDI stream or UART — same byte format.
// Emits note on/off, CC, pitch bend, and program change events.
struct MidiParser {
    uint8_t status;    // current running status byte
    uint8_t data[2];   // data byte accumulator
    uint8_t count;     // data bytes received so far

    void init() {
        status = 0;
        count = 0;
    }

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

        // System common (0xF0-0xF7): cancel running status
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
