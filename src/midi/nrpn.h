#pragma once

#include <cstdint>

// Per-channel NRPN state machine: CC99/CC98 select a 14-bit parameter
// number, CC6 (Data Entry MSB) and CC38 (Data Entry LSB) carry its value.
// An RPN select (CC101/CC100) deselects, since RPNs aren't NRPNs; so does
// the null parameter 127/127.
//
// A Data Entry MSB alone yields a 14-bit value with the MSB replicated into
// the low bits (127 -> 16383), so a 7-bit controller reaches full scale; a
// following LSB yields the exact 14-bit value. Selecting a parameter resets
// the MSB, so an LSB alone never combines with another parameter's MSB.
struct NrpnDecoder {
    static constexpr uint16_t kNull = 0x3FFF;

    uint16_t param = kNull;
    uint8_t data_msb = 0;

    static constexpr bool is_nrpn_cc(uint8_t cc) {
        return cc == 99 || cc == 98 || cc == 101 || cc == 100 || cc == 6 || cc == 38;
    }

    // Feed one of the is_nrpn_cc() controllers. True when it completes a
    // value for a selected parameter, returned in `param_out`/`value14`.
    bool feed(uint8_t cc, uint8_t value, uint16_t &param_out, uint16_t &value14) {
        switch (cc) {
            case 99: param = (uint16_t)((value << 7) | (param & 0x7F)); data_msb = 0; return false;
            case 98: param = (uint16_t)((param & 0x3F80) | value); data_msb = 0; return false;
            case 101:
            case 100: param = kNull; data_msb = 0; return false;
            case 6:
                data_msb = value;
                value14 = (uint16_t)((value << 7) | value);
                break;
            case 38:
                value14 = (uint16_t)((data_msb << 7) | value);
                break;
            default: return false;
        }
        if (param == kNull) return false;
        param_out = param;
        return true;
    }
};
