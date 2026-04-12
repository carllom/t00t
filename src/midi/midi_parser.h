#pragma once

#include <cstdint>

// MIDI event types emitted by the parser
enum MidiMsgType : uint8_t {
    MIDI_NOTE_OFF = 0,
    MIDI_NOTE_ON  = 1,
};

struct MidiEvent {
    MidiMsgType type;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
};

// Transport-agnostic MIDI byte-stream parser.
// Handles running status and system real-time pass-through.
// Feeds from USB MIDI stream or UART — same byte format.
// Currently emits only note on/off events.
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
        // System real-time (0xF8-0xFF): pass through, don't affect state
        if (byte >= 0xF8) return false;

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
        if (hi == 0x90 && data[1] > 0) {
            out = { MIDI_NOTE_ON, (uint8_t)(status & 0x0F), data[0], data[1] };
            return true;
        }
        if (hi == 0x80 || (hi == 0x90 && data[1] == 0)) {
            out = { MIDI_NOTE_OFF, (uint8_t)(status & 0x0F), data[0], data[1] };
            return true;
        }

        return false;  // CC, pitch bend, etc. — ignored for now
    }
};
