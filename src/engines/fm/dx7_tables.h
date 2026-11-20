#pragma once

#include "patch.h"
#include <cstdint>

// DX7 voice data -> this engine's patch form, the constant tables. Written
// out from tools/syx2patch.py, which converts the same data on the host
// (test_syx2patch.py checks the two agree), so a Patch cooked on the device
// matches the one the converter emits.

// One operator's place in a DX7 algorithm, in this engine's terms
// (FmOpParams): its modulation target, the further operators it also
// modulates, and whether it is the algorithm's feedback operator.
struct Dx7AlgorithmOp {
    uint8_t mod_target;         // 0-5, or FM_TARGET_OUT
    uint8_t extra_target_mask;
    bool feedback;
};

struct Dx7Algorithm {
    Dx7AlgorithmOp op[FM_NUM_OPS];  // index 0 = OP1
};

// The 32 DX7 algorithms (index = ALGORITHM value, 0 = "algorithm 1"),
// decoded from Dexed's bus flags by syx2patch.py's decode_algorithm().
// Algorithms 4 and 6 keep only their primary feedback loop.
inline constexpr Dx7Algorithm kDx7Algorithms[32] = {
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 3, 0x00, false }, { 4, 0x00, true } } },  // 1
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, true }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 3, 0x00, false }, { 4, 0x00, false } } },  // 2
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 1, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 4, 0x00, true } } },  // 3
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 1, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 4, 0x00, true } } },  // 4
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x00, true } } },  // 5
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x00, true } } },  // 6
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 2, 0x00, false }, { 4, 0x00, true } } },  // 7
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, true }, { 2, 0x00, false }, { 4, 0x00, false } } },  // 8
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, true }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 2, 0x00, false }, { 4, 0x00, false } } },  // 9
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 1, 0x00, true }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, false } } },  // 10
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 1, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, true } } },  // 11
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, true }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 2, 0x00, false }, { 2, 0x00, false } } },  // 12
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 2, 0x00, false }, { 2, 0x00, true } } },  // 13
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, true } } },  // 14
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, true }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, false } } },  // 15
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 0, 0x00, false }, { 2, 0x00, false }, { 0, 0x00, false }, { 4, 0x00, true } } },  // 16
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, true }, { 0, 0x00, false }, { 2, 0x00, false }, { 0, 0x00, false }, { 4, 0x00, false } } },  // 17
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 0, 0x00, true }, { 0, 0x00, false }, { 3, 0x00, false }, { 4, 0x00, false } } },  // 18
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { 1, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x08, true } } },  // 19
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 1, 0x01, true }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, false } } },  // 20
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 1, 0x01, true }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x08, false } } },  // 21
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x0C, true } } },  // 22
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 1, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x08, true } } },  // 23
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x0C, true } } },  // 24
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x08, true } } },  // 25
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 1, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, true } } },  // 26
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 1, 0x00, true }, { FM_TARGET_OUT, 0x00, false }, { 3, 0x00, false }, { 3, 0x00, false } } },  // 27
    {{ { FM_TARGET_OUT, 0x00, false }, { 0, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 3, 0x00, true }, { FM_TARGET_OUT, 0x00, false } } },  // 28
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x00, true } } },  // 29
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 2, 0x00, false }, { 3, 0x00, true }, { FM_TARGET_OUT, 0x00, false } } },  // 30
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { 4, 0x00, true } } },  // 31
    {{ { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, false }, { FM_TARGET_OUT, 0x00, true } } },  // 32
};

// Fixed-frequency mode: Hz for (COARSE & 3) * 100 + FINE, i.e.
// 10^(n / 100), to 6 decimals as the converter writes it.
static constexpr uint32_t kDx7FixedHzCount = 3 * 100 + 127 + 1;
inline constexpr float kDx7FixedHz[kDx7FixedHzCount] = {
    1.000000f, 1.023293f, 1.047129f, 1.071519f, 1.096478f, 1.122018f,
    1.148154f, 1.174898f, 1.202264f, 1.230269f, 1.258925f, 1.288250f,
    1.318257f, 1.348963f, 1.380384f, 1.412538f, 1.445440f, 1.479108f,
    1.513561f, 1.548817f, 1.584893f, 1.621810f, 1.659587f, 1.698244f,
    1.737801f, 1.778279f, 1.819701f, 1.862087f, 1.905461f, 1.949845f,
    1.995262f, 2.041738f, 2.089296f, 2.137962f, 2.187762f, 2.238721f,
    2.290868f, 2.344229f, 2.398833f, 2.454709f, 2.511886f, 2.570396f,
    2.630268f, 2.691535f, 2.754229f, 2.818383f, 2.884032f, 2.951209f,
    3.019952f, 3.090295f, 3.162278f, 3.235937f, 3.311311f, 3.388442f,
    3.467369f, 3.548134f, 3.630781f, 3.715352f, 3.801894f, 3.890451f,
    3.981072f, 4.073803f, 4.168694f, 4.265795f, 4.365158f, 4.466836f,
    4.570882f, 4.677351f, 4.786301f, 4.897788f, 5.011872f, 5.128614f,
    5.248075f, 5.370318f, 5.495409f, 5.623413f, 5.754399f, 5.888437f,
    6.025596f, 6.165950f, 6.309573f, 6.456542f, 6.606934f, 6.760830f,
    6.918310f, 7.079458f, 7.244360f, 7.413102f, 7.585776f, 7.762471f,
    7.943282f, 8.128305f, 8.317638f, 8.511380f, 8.709636f, 8.912509f,
    9.120108f, 9.332543f, 9.549926f, 9.772372f, 10.000000f, 10.232930f,
    10.471285f, 10.715193f, 10.964782f, 11.220185f, 11.481536f, 11.748976f,
    12.022644f, 12.302688f, 12.589254f, 12.882496f, 13.182567f, 13.489629f,
    13.803843f, 14.125375f, 14.454398f, 14.791084f, 15.135612f, 15.488166f,
    15.848932f, 16.218101f, 16.595869f, 16.982437f, 17.378008f, 17.782794f,
    18.197009f, 18.620871f, 19.054607f, 19.498446f, 19.952623f, 20.417379f,
    20.892961f, 21.379621f, 21.877616f, 22.387211f, 22.908677f, 23.442288f,
    23.988329f, 24.547089f, 25.118864f, 25.703958f, 26.302680f, 26.915348f,
    27.542287f, 28.183829f, 28.840315f, 29.512092f, 30.199517f, 30.902954f,
    31.622777f, 32.359366f, 33.113112f, 33.884416f, 34.673685f, 35.481339f,
    36.307805f, 37.153523f, 38.018940f, 38.904514f, 39.810717f, 40.738028f,
    41.686938f, 42.657952f, 43.651583f, 44.668359f, 45.708819f, 46.773514f,
    47.863009f, 48.977882f, 50.118723f, 51.286138f, 52.480746f, 53.703180f,
    54.954087f, 56.234133f, 57.543994f, 58.884366f, 60.255959f, 61.659500f,
    63.095734f, 64.565423f, 66.069345f, 67.608298f, 69.183097f, 70.794578f,
    72.443596f, 74.131024f, 75.857758f, 77.624712f, 79.432823f, 81.283052f,
    83.176377f, 85.113804f, 87.096359f, 89.125094f, 91.201084f, 93.325430f,
    95.499259f, 97.723722f, 100.000000f, 102.329299f, 104.712855f, 107.151931f,
    109.647820f, 112.201845f, 114.815362f, 117.489755f, 120.226443f, 123.026877f,
    125.892541f, 128.824955f, 131.825674f, 134.896288f, 138.038426f, 141.253754f,
    144.543977f, 147.910839f, 151.356125f, 154.881662f, 158.489319f, 162.181010f,
    165.958691f, 169.824365f, 173.780083f, 177.827941f, 181.970086f, 186.208714f,
    190.546072f, 194.984460f, 199.526231f, 204.173794f, 208.929613f, 213.796209f,
    218.776162f, 223.872114f, 229.086765f, 234.422882f, 239.883292f, 245.470892f,
    251.188643f, 257.039578f, 263.026799f, 269.153480f, 275.422870f, 281.838293f,
    288.403150f, 295.120923f, 301.995172f, 309.029543f, 316.227766f, 323.593657f,
    331.131121f, 338.844156f, 346.736850f, 354.813389f, 363.078055f, 371.535229f,
    380.189396f, 389.045145f, 398.107171f, 407.380278f, 416.869383f, 426.579519f,
    436.515832f, 446.683592f, 457.088190f, 467.735141f, 478.630092f, 489.778819f,
    501.187234f, 512.861384f, 524.807460f, 537.031796f, 549.540874f, 562.341325f,
    575.439937f, 588.843655f, 602.559586f, 616.595002f, 630.957344f, 645.654229f,
    660.693448f, 676.082975f, 691.830971f, 707.945784f, 724.435960f, 741.310241f,
    758.577575f, 776.247117f, 794.328235f, 812.830516f, 831.763771f, 851.138038f,
    870.963590f, 891.250938f, 912.010839f, 933.254301f, 954.992586f, 977.237221f,
    1000.000000f, 1023.292992f, 1047.128548f, 1071.519305f, 1096.478196f, 1122.018454f,
    1148.153621f, 1174.897555f, 1202.264435f, 1230.268771f, 1258.925412f, 1288.249552f,
    1318.256739f, 1348.962883f, 1380.384265f, 1412.537545f, 1445.439771f, 1479.108388f,
    1513.561248f, 1548.816619f, 1584.893192f, 1621.810097f, 1659.586907f, 1698.243652f,
    1737.800829f, 1778.279410f, 1819.700859f, 1862.087137f, 1905.460718f, 1949.844600f,
    1995.262315f, 2041.737945f, 2089.296131f, 2137.962090f, 2187.761624f, 2238.721139f,
    2290.867653f, 2344.228815f, 2398.832919f, 2454.708916f, 2511.886432f, 2570.395783f,
    2630.267992f, 2691.534804f, 2754.228703f, 2818.382931f, 2884.031503f, 2951.209227f,
    3019.951720f, 3090.295433f, 3162.277660f, 3235.936569f, 3311.311215f, 3388.441561f,
    3467.368505f, 3548.133892f, 3630.780548f, 3715.352291f, 3801.893963f, 3890.451450f,
    3981.071706f, 4073.802778f, 4168.693835f, 4265.795188f, 4365.158322f, 4466.835922f,
    4570.881896f, 4677.351413f, 4786.300923f, 4897.788194f, 5011.872336f, 5128.613840f,
    5248.074602f, 5370.317964f, 5495.408739f, 5623.413252f, 5754.399373f, 5888.436554f,
    6025.595861f, 6165.950019f, 6309.573445f, 6456.542290f, 6606.934480f, 6760.829754f,
    6918.309709f, 7079.457844f, 7244.359601f, 7413.102413f, 7585.775750f, 7762.471166f,
    7943.282347f, 8128.305162f, 8317.637711f, 8511.380382f, 8709.635900f, 8912.509381f,
    9120.108394f, 9332.543008f, 9549.925860f, 9772.372210f, 10000.000000f, 10232.929923f,
    10471.285481f, 10715.193052f, 10964.781961f, 11220.184543f, 11481.536215f, 11748.975549f,
    12022.644346f, 12302.687708f, 12589.254118f, 12882.495517f, 13182.567386f, 13489.628826f,
    13803.842646f, 14125.375446f, 14454.397707f, 14791.083882f, 15135.612484f, 15488.166189f,
    15848.931925f, 16218.100974f, 16595.869074f, 16982.436525f, 17378.008287f, 17782.794100f,
    18197.008586f, 18620.871367f,
};
