#include "samples.h"
#include "../samples/sararr1.h"
#include "../samples/mallets/marimba.h"
#include "../samples/strings/lowstr5.h"
#include "../samples/voiceiix/ah2.h"
#include "../samples/voiceiix/arr.h"
#include "../samples/voiceiix/rrr.h"

const SampleDef sararr1_sample = {
    sararr1_data,
    sararr1_NUM_SAMPLES,
    sararr1_SAMPLE_RATE,
    440.0f,
    (bool)sararr1_LOOPED,
    sararr1_LOOP_START,
    sararr1_LOOP_END
};

const SampleDef marimba_sample = {
    marimba_data,
    marimba_NUM_SAMPLES,
    marimba_SAMPLE_RATE,
    440.0f,
    (bool)marimba_LOOPED,
    marimba_LOOP_START,
    marimba_LOOP_END
};

const SampleDef lowstr5_sample = {
    lowstr5_data,
    lowstr5_NUM_SAMPLES,
    lowstr5_SAMPLE_RATE,
    440.0f,
    (bool)lowstr5_LOOPED,
    lowstr5_LOOP_START,
    lowstr5_LOOP_END
};

const SampleDef ah2_sample = {
    ah2_data,
    ah2_NUM_SAMPLES,
    ah2_SAMPLE_RATE,
    440.0f,
    (bool)ah2_LOOPED,
    ah2_LOOP_START,
    ah2_LOOP_END
};

const SampleDef arr_sample = {
    arr_data,
    arr_NUM_SAMPLES,
    arr_SAMPLE_RATE,
    440.0f,
    (bool)arr_LOOPED,
    arr_LOOP_START,
    arr_LOOP_END
};

const SampleDef rrr_sample = {
    rrr_data,
    rrr_NUM_SAMPLES,
    rrr_SAMPLE_RATE,
    440.0f,
    (bool)rrr_LOOPED,
    rrr_LOOP_START,
    rrr_LOOP_END
};
