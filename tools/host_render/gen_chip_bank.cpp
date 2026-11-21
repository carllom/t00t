// Generates src/engines/chip/factory_bank.h -- the chip factory bank as
// stored-format Patches -- from the hand-authored instrument tables in
// src/engines/chip/instruments.h (SID) and ay_instruments.h (AY), SID's
// first, so the program numbers are the instruments' combined indices.
//
//   gen_chip_bank <out.h>            # write the header
//   gen_chip_bank --check <file.h>   # exit 1 if <file.h> is stale
//
// Each instrument is converted to Descriptor values, and the conversion is
// checked to be lossless: cooking the values must give back the
// instrument's own fields and table rows.

#include "chip_module.h"
#include "instruments.h"
#include "ay_instruments.h"
#include "patch/stored_format.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool g_ok = true;
std::string g_name;

void fail(const char *what) {
    fprintf(stderr, "%s: %s\n", g_name.c_str(), what);
    g_ok = false;
}

void defaults(int32_t (&v)[CHIP_P_COUNT]) {
    for (uint32_t p = 0; p < CHIP_P_COUNT; p++) v[p] = ChipModule::kDescriptors[p].def;
}

int32_t wave_row_bits(const WaveRow &r) {
    int32_t bits = (r.waveform == WAVE_HOLD) ? CHIP_ROW_HOLD : r.waveform;
    if (r.waveform != WAVE_HOLD && (r.waveform & ~CHIP_ROW_WAVE_MASK)) fail("waveform outside the SidWave bits");
    if (r.flags & WAVE_FLAG_SYNC) bits |= CHIP_ROW_SYNC;
    if (r.flags & WAVE_FLAG_RING) bits |= CHIP_ROW_RING;
    return bits;
}

void put_sweep(int32_t (&v)[CHIP_P_COUNT], uint16_t len_id, uint16_t rows_id, const SweepTable &t) {
    if (t.len > CHIP_TABLE_ROWS) fail("a sweep table longer than a Patch holds");
    v[len_id] = t.len;
    v[len_id + 1] = t.loop > CHIP_TABLE_ROWS ? CHIP_TABLE_ROWS : t.loop;  // past the end: hold the last row
    for (uint32_t r = 0; r < t.len && r < CHIP_TABLE_ROWS; r++) {
        v[rows_id + 2 * r] = t.rows[r].delta;
        v[rows_id + 2 * r + 1] = t.rows[r].duration;
    }
}

void sid_values(const Instrument &i, int32_t (&v)[CHIP_P_COUNT]) {
    defaults(v);
    v[CHIP_P_TYPE] = 0;
    v[CHIP_P_ATTACK] = i.ad >> 4;
    v[CHIP_P_DECAY] = i.ad & 15;
    v[CHIP_P_SUSTAIN] = i.sr >> 4;
    v[CHIP_P_RELEASE] = i.sr & 15;
    v[CHIP_P_VIB_DEPTH] = i.vibrato_depth;
    v[CHIP_P_VIB_SPEED] = i.vibrato_speed;
    v[CHIP_P_VIB_DELAY] = i.vibrato_delay;
    v[CHIP_P_GATE_OFF] = i.gate_off_timer;
    v[CHIP_P_FIRST_WAVE] = i.first_wave;
    v[CHIP_P_PULSE_INIT] = i.pulse_init;
    if (i.uses_filter != (i.filter_mode_mask != 0)) fail("uses_filter disagrees with the filter mode bits");
    v[CHIP_P_FILTER] = i.filter_mode_mask;
    v[CHIP_P_CUTOFF] = i.filter_cutoff_init;
    v[CHIP_P_RESONANCE] = i.filter_resonance;
    if (i.wave.len > CHIP_TABLE_ROWS) fail("a wave table longer than a Patch holds");
    v[CHIP_P_WAVE_LEN] = i.wave.len;
    v[CHIP_P_WAVE_LOOP] = i.wave.loop > CHIP_TABLE_ROWS ? CHIP_TABLE_ROWS : i.wave.loop;
    for (uint32_t r = 0; r < i.wave.len && r < CHIP_TABLE_ROWS; r++) {
        v[chip_wave_row_wave(r)] = wave_row_bits(i.wave.rows[r]);
        v[chip_wave_row_note(r)] = i.wave.rows[r].note;
    }
    put_sweep(v, CHIP_P_PULSE_LEN, CHIP_P_PULSE_ROWS, i.pulse);
    put_sweep(v, CHIP_P_FILTER_LEN, CHIP_P_FILTER_ROWS, i.filter);
}

void ay_values(const AyInstrument &i, int32_t (&v)[CHIP_P_COUNT]) {
    defaults(v);
    v[CHIP_P_TYPE] = 1;
    v[CHIP_P_VIB_DEPTH] = i.vibrato_depth;
    v[CHIP_P_VIB_SPEED] = i.vibrato_speed;
    v[CHIP_P_VIB_DELAY] = i.vibrato_delay;
    v[CHIP_P_GATE_OFF] = i.gate_off_timer;
    v[CHIP_P_AY_MODEL] = i.model == AY_MODEL_YM2149 ? 1 : 0;
    v[CHIP_P_AY_VOLUME] = i.initial_volume;
    v[CHIP_P_AY_LEVEL] = i.use_envelope ? 1 : 0;
    v[CHIP_P_AY_ENV_PERIOD] = i.envelope_period;
    v[CHIP_P_AY_ENV_SHAPE] = i.envelope_shape;
    v[CHIP_P_AY_MIXER] = (i.tone_on ? 1 : 0) | (i.noise_on ? 2 : 0);
    v[CHIP_P_AY_NOISE_PERIOD] = i.noise_period;
    if (i.tone.len > CHIP_TABLE_ROWS) fail("a tone table longer than a Patch holds");
    v[CHIP_P_WAVE_LEN] = i.tone.len;
    v[CHIP_P_WAVE_LOOP] = i.tone.loop > CHIP_TABLE_ROWS ? CHIP_TABLE_ROWS : i.tone.loop;
    for (uint32_t r = 0; r < i.tone.len && r < CHIP_TABLE_ROWS; r++) v[chip_wave_row_note(r)] = i.tone.rows[r].note;
    put_sweep(v, CHIP_P_PULSE_LEN, CHIP_P_PULSE_ROWS, i.volume);
}

void check_range(const int32_t (&v)[CHIP_P_COUNT]) {
    for (uint32_t p = 0; p < CHIP_P_COUNT; p++) {
        const ParamDescriptor &d = ChipModule::kDescriptors[p];
        if (v[p] < d.min || v[p] > d.max) fail(d.name);
    }
}

// Table loop points past the end all mean "hold the last row".
bool same_loop(uint8_t cooked, uint8_t src, uint8_t len) {
    return cooked == src || (cooked >= len && src >= len);
}

void check_sweep(const ChipSweepTable &c, const SweepTable &t, const char *what) {
    bool ok = c.len == t.len && same_loop(c.loop, t.loop, t.len);
    for (uint32_t r = 0; ok && r < t.len; r++)
        ok = c.rows[r].delta == t.rows[r].delta && c.rows[r].duration == t.rows[r].duration;
    if (!ok) fail(what);
}

void check_sid(const Instrument &i, const ChipCookedPatch &c) {
    if (c.type != VT_SID || c.ad != i.ad || c.sr != i.sr || c.vibrato_depth != i.vibrato_depth ||
        c.vibrato_speed != i.vibrato_speed || c.vibrato_delay != i.vibrato_delay ||
        c.gate_off_timer != i.gate_off_timer || c.first_wave != i.first_wave || c.pulse_init != i.pulse_init ||
        c.uses_filter != i.uses_filter || c.filter_mode_mask != i.filter_mode_mask ||
        c.filter_cutoff_init != i.filter_cutoff_init || c.filter_resonance != i.filter_resonance)
        fail("cooked instrument settings");
    bool ok = c.wave.len == i.wave.len && same_loop(c.wave.loop, i.wave.loop, i.wave.len);
    for (uint32_t r = 0; ok && r < i.wave.len; r++) {
        const WaveRow &a = c.wave.rows[r], &b = i.wave.rows[r];
        ok = a.waveform == b.waveform && a.flags == b.flags && a.note == b.note;
    }
    if (!ok) fail("cooked wave table");
    check_sweep(c.pulse, i.pulse, "cooked pulse table");
    check_sweep(c.filter, i.filter, "cooked filter table");
    if (i.hard_restart) fail("hard_restart is set, but the engine never reads it and a Patch doesn't store it");
}

void check_ay(const AyInstrument &i, const ChipCookedPatch &c) {
    if (c.type != VT_AY || c.ay_model != i.model || c.ay_initial_volume != i.initial_volume ||
        c.ay_use_envelope != i.use_envelope || c.ay_envelope_period != i.envelope_period ||
        c.ay_envelope_shape != i.envelope_shape || c.ay_tone_on != i.tone_on || c.ay_noise_on != i.noise_on ||
        c.ay_noise_period != i.noise_period || c.vibrato_depth != i.vibrato_depth ||
        c.vibrato_speed != i.vibrato_speed || c.vibrato_delay != i.vibrato_delay ||
        c.gate_off_timer != i.gate_off_timer)
        fail("cooked instrument settings");
    bool ok = c.wave.len == i.tone.len && same_loop(c.wave.loop, i.tone.loop, i.tone.len);
    for (uint32_t r = 0; ok && r < i.tone.len; r++) ok = c.wave.rows[r].note == i.tone.rows[r].note;
    if (!ok) fail("cooked tone table");
    check_sweep(c.pulse, i.volume, "cooked volume table");
}

// "ARP_LEAD" -> "ARP LEAD"; AY's get an "AY " prefix.
std::string patch_name(const char *src, bool ay) {
    std::string n = ay ? std::string("AY ") + src : std::string(src);
    for (char &ch : n) if (ch == '_') ch = ' ';
    return n;
}

std::string generate() {
    std::string out =
        "// GENERATED by tools/host_render/gen_chip_bank.cpp from instruments.h and ay_instruments.h\n"
        "// -- do not hand-edit. The chip factory bank: stored-format Patches, compiled in, read-only.\n"
        "#pragma once\n\n"
        "#include \"patch/stored_format.h\"\n"
        "#include <cstdint>\n\n";

    constexpr uint32_t kSize = patch_stored_size(ChipModule::kDescriptors);
    const uint32_t count = INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT;
    char line[160];
    for (uint32_t k = 0; k < count; k++) {
        const bool ay = k >= INSTRUMENT_COUNT;
        const std::string name =
            patch_name(ay ? AY_INSTRUMENT_NAMES[k - INSTRUMENT_COUNT] : INSTRUMENT_NAMES[k], ay);
        g_name = name;
        int32_t v[CHIP_P_COUNT];
        if (ay) ay_values(AY_INSTRUMENTS[k - INSTRUMENT_COUNT], v);
        else sid_values(INSTRUMENTS[k], v);
        check_range(v);
        ChipCookedPatch c{};
        ChipModule::cook(v, c);
        if (ay) check_ay(AY_INSTRUMENTS[k - INSTRUMENT_COUNT], c);
        else check_sid(INSTRUMENTS[k], c);
        if (name.size() > kPatchNameLen) fail("name longer than a stored name");

        uint8_t bytes[kSize];
        patch_encode(ChipModule::kModuleId, ChipModule::kSchemaVersion, name.c_str(), ChipModule::kDescriptors, v,
                     bytes, kSize);
        snprintf(line, sizeof line, "// %u: %s\nstatic const uint8_t kChipFactoryPatch%u[] = {", k, name.c_str(), k);
        out += line;
        for (uint32_t b = 0; b < kSize; b++) {
            snprintf(line, sizeof line, "%s0x%02x,", (b % 12 == 0) ? "\n    " : " ", bytes[b]);
            out += line;
        }
        out += "\n};\n\n";
    }
    out += "static const StoredPatchRef kChipFactoryBank[] = {\n";
    for (uint32_t k = 0; k < count; k++) {
        snprintf(line, sizeof line, "    { kChipFactoryPatch%u, sizeof kChipFactoryPatch%u },\n", k, k);
        out += line;
    }
    out += "};\n";
    snprintf(line, sizeof line, "static constexpr uint32_t kChipFactoryCount = %u;\n", count);
    out += line;
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    bool check = argc == 3 && !strcmp(argv[1], "--check");
    if (!(argc == 2 || check)) {
        fprintf(stderr, "usage: %s <out.h> | --check <file.h>\n", argv[0]);
        return 2;
    }
    const char *path = argv[argc - 1];
    std::string text = generate();
    if (!g_ok) return 1;

    if (check) {
        FILE *f = fopen(path, "rb");
        if (!f) { perror(path); return 1; }
        std::string existing;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) existing.append(buf, n);
        fclose(f);
        bool fresh = existing == text;
        printf(fresh ? "factory bank up to date\nALL CHECKS PASSED\n"
                     : "FAIL %s is stale -- regenerate with gen_chip_bank\nCHECKS FAILED\n", path);
        return fresh ? 0 : 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    printf("wrote %u patches to %s\n", (unsigned)(INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT), path);
    return 0;
}
