#pragma once

#include <cstdint>

// Shared fractional-delay allpass coefficient for flanger.h/chorus.h's
// interpolated taps. For a tap split into an integer delay N and a
// fractional remainder `frac` in [0,1) between the N and N+1 samples, the
// correct first-order allpass interpolator coefficient is
// eta = (1-frac)/(1+frac) -- not `frac` itself, which runs the wrong
// direction (coefficient -> 1, the interpolator's least accurate point,
// exactly when frac -> 1 i.e. right as the sweep crosses each sample
// boundary) and was audible as warble/ringing on the flanger sweep.
// Table-driven instead of a per-sample division: frac arrives here as the
// 8-bit fractional part of a Q8 delay length, so a 256-entry Q15 LUT covers
// it exactly at the same one-lookup cost as reading `frac` directly.
static const int16_t ALLPASS_ETA_Q15[256] = {
    32767, 32512, 32259, 32008, 31759, 31512, 31266, 31023,
    30781, 30541, 30303, 30067, 29833, 29600, 29369, 29140,
    28912, 28686, 28462, 28239, 28018, 27799, 27581, 27365,
    27150, 26937, 26725, 26515, 26306, 26099, 25893, 25688,
    25485, 25284, 25084, 24885, 24687, 24491, 24297, 24103,
    23911, 23720, 23531, 23342, 23155, 22970, 22785, 22602,
    22420, 22239, 22059, 21880, 21703, 21527, 21351, 21177,
    21004, 20833, 20662, 20492, 20324, 20156, 19990, 19825,
    19660, 19497, 19335, 19173, 19013, 18854, 18695, 18538,
    18381, 18226, 18071, 17918, 17765, 17613, 17463, 17313,
    17164, 17016, 16868, 16722, 16576, 16432, 16288, 16145,
    16002, 15861, 15721, 15581, 15442, 15304, 15166, 15030,
    14894, 14759, 14625, 14491, 14359, 14227, 14095, 13965,
    13835, 13706, 13577, 13450, 13323, 13197, 13071, 12946,
    12822, 12698, 12575, 12453, 12332, 12211, 12090, 11971,
    11852, 11734, 11616, 11499, 11382, 11266, 11151, 11036,
    10922, 10809, 10696, 10584, 10472, 10361, 10250, 10140,
    10031, 9922, 9813, 9706, 9598, 9492, 9386, 9280,
    9175, 9070, 8966, 8863, 8759, 8657, 8555, 8453,
    8352, 8252, 8152, 8052, 7953, 7855, 7756, 7659,
    7562, 7465, 7369, 7273, 7178, 7083, 6988, 6894,
    6801, 6708, 6615, 6523, 6431, 6340, 6249, 6158,
    6068, 5978, 5889, 5800, 5712, 5624, 5536, 5449,
    5362, 5275, 5189, 5104, 5018, 4933, 4849, 4765,
    4681, 4598, 4515, 4432, 4350, 4268, 4186, 4105,
    4024, 3944, 3863, 3784, 3704, 3625, 3546, 3468,
    3390, 3312, 3235, 3157, 3081, 3004, 2928, 2852,
    2777, 2702, 2627, 2552, 2478, 2404, 2331, 2257,
    2184, 2112, 2039, 1967, 1896, 1824, 1753, 1682,
    1611, 1541, 1471, 1401, 1332, 1263, 1194, 1125,
    1057, 989, 921, 854, 786, 719, 653, 586,
    520, 454, 389, 323, 258, 193, 128, 64,
};

inline int32_t allpass_eta_q15(uint32_t frac_q8) {
    return ALLPASS_ETA_Q15[frac_q8 & 0xFF];
}
