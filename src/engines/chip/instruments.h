#pragma once

#include "instrument.h"
#include "chip/sid_filter.h"   // SID_FILT_LP/BP/HP

// GENERATED FILE -- do not edit by hand.
// Source: tools/chip_instruments.txt
// Regenerate: tools/chipgen.py gen <source.txt> <this file>
// (chip.md §6, §11.2 "hand-authored text -> generator script")

static const WaveRow ARP_LEAD_WAVE[] = {
    { 0x2, 0, 0 },
    { 0x2, 0, 0 },
    { 0x2, 0, 4 },
    { 0x2, 0, 4 },
    { 0x2, 0, 7 },
    { 0x2, 0, 7 },
    { 0x2, 0, 12 },
    { 0x2, 0, 12 },
};

static const SweepRow PWM_PLUCK_PULSE[] = {
    { 90, 32 },
    { 45, 4 },
    { 20, 2 },
    { 8, 2 },
    { 0, 250 },
};

static const SweepRow FILTER_PAD_FILTER[] = {
    { 30, 60 },
    { 0, 250 },
};


enum ChipInstrumentId : uint8_t {
    INS_ARP_LEAD,
    INS_PWM_PLUCK,
    INS_FILTER_PAD,
    INS_VIBRATO_LEAD,
    INSTRUMENT_COUNT,
};

static const char *const INSTRUMENT_NAMES[INSTRUMENT_COUNT] = {
    "ARP_LEAD",
    "PWM_PLUCK",
    "FILTER_PAD",
    "VIBRATO_LEAD",
};

static const Instrument INSTRUMENTS[INSTRUMENT_COUNT] = {
    // INS_ARP_LEAD
    {
        0x16, 0xa5, 0,  0, 0, 0,  0,  0x2,
        { ARP_LEAD_WAVE, 8, 0 },
        { nullptr, 0, 0 }, 0,
        false, 0, 0, 0x0, { nullptr, 0, 0 },
    },
    // INS_PWM_PLUCK
    {
        0x04, 0x43, 0,  0, 0, 0,  45,  0x4,
        { nullptr, 0, 0 },
        { PWM_PLUCK_PULSE, 5, 4 }, 200,
        false, 0, 0, 0x0, { nullptr, 0, 0 },
    },
    // INS_FILTER_PAD
    {
        0x99, 0xca, 0,  15, 40, 20,  0,  0x6,
        { nullptr, 0, 0 },
        { nullptr, 0, 0 }, 2048,
        true, 200, 8, 0x1, { FILTER_PAD_FILTER, 2, 1 },
    },
    // INS_VIBRATO_LEAD
    {
        0x37, 0xb8, 0,  40, 110, 15,  0,  0x2,
        { nullptr, 0, 0 },
        { nullptr, 0, 0 }, 0,
        false, 0, 0, 0x0, { nullptr, 0, 0 },
    },
};
