// Host-buildable unit test for src/input_layer.h (#84/#85 tracer bullet,
// issue #86). Pure per-voice-independent dispatch/capability logic, no
// audio math and no hardware dependency -- same rationale and convention as
// render_xm_device.cpp's no-arg invocation mode (test_*() functions, an
// aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").
//
// Deliberately does not exercise any real engine's midi_controller.cpp --
// those pull in pico-sdk-dependent headers and stay verified on real
// hardware, per #85's Testing Decisions. This covers only input_layer.h's
// own mechanism: table matching, the generic mutators, and the compile-time
// capability check.

#include "../../src/input_layer.h"

#include <cstdio>

namespace {

// --- A minimal Context + setters, just enough to observe dispatch. ---

struct TestContext {
    int calls = 0;
    InputCategory last_category{};
    uint8_t last_channel = 0;
    uint8_t last_velocity = 0;
    float last_scalar = 0.0f;
    int global_target = -1;  // simulates a module-global write (e.g. shadow.fx.*),
                              // distinct from a per-voice/per-channel one -- there is
                              // no dispatch-level "scope" concept, so this is purely
                              // a fact about what set_fx_like below chooses to write
};

void set_note(TestContext &ctx, const InputValue &v) {
    ctx.calls++;
    ctx.last_category = InputCategory::NOTE;
    ctx.last_channel = v.channel;
    ctx.last_velocity = v.velocity;
}

void set_modifier(TestContext &ctx, const InputValue &v) {
    ctx.calls++;
    ctx.last_category = InputCategory::MODIFIER;
    ctx.last_channel = v.channel;
    ctx.last_scalar = v.scalar;
}

// A module-global Modifier setter (mirrors subtractive's FX CC setters,
// which write shadow.fx.* -- one instance per VoiceParamBlock, not
// per-voice). Nothing about the dispatch mechanism needs to know this
// setter's target is global rather than per-voice/per-channel: it's purely
// the setter's own choice of what to write, same call shape as set_modifier.
void set_fx_like(TestContext &ctx, const InputValue &v) {
    ctx.calls++;
    ctx.last_category = InputCategory::MODIFIER;
    ctx.last_channel = v.channel;
    ctx.global_target = (int)v.scalar;
}

constexpr InputMapEntryT<TestContext> kTable[] = {
    { InputCategory::NOTE,     60,  72,  0xFF, 0,   set_note },      // one-octave note range
    { InputCategory::MODIFIER, 1,   1,   0xFF, 0,   set_modifier },  // CC1, any channel
    { InputCategory::MODIFIER, 10,  10,  5,    0,   set_modifier },  // CC10, channel 5 only
    { InputCategory::NOTE,     90,  90,  0xFF, 100, set_note },      // fixed velocity 100
    { InputCategory::MODIFIER, 74,  74,  0xFF, 0,   set_fx_like },   // CC74-style, module-global, any channel
};

bool test_dispatch_matches_category_and_id() {
    TestContext ctx;
    InputValue value{};
    value.channel = 3;
    value.scalar = 42.0f;
    input_dispatch(ctx, kTable, InputCategory::MODIFIER, 1, value);

    bool ok = ctx.calls == 1 && ctx.last_category == InputCategory::MODIFIER &&
              ctx.last_channel == 3 && ctx.last_scalar == 42.0f;
    printf(ok ? "  OK: CC1 dispatches to its Modifier setter\n"
              : "  FAIL: CC1 dispatch produced wrong call/values\n");
    return ok;
}

bool test_no_match_is_noop() {
    TestContext ctx;
    InputValue value{};
    input_dispatch(ctx, kTable, InputCategory::MODIFIER, 99, value);  // no entry has id 99

    bool ok = ctx.calls == 0;
    printf(ok ? "  OK: unmatched id dispatches nothing\n"
              : "  FAIL: unmatched id invoked a setter\n");
    return ok;
}

bool test_wrong_category_is_noop() {
    TestContext ctx;
    InputValue value{};
    // id 1 exists in the table, but only tagged MODIFIER -- NOTE must not match it.
    input_dispatch(ctx, kTable, InputCategory::NOTE, 1, value);

    bool ok = ctx.calls == 0;
    printf(ok ? "  OK: matching id under the wrong category dispatches nothing\n"
              : "  FAIL: wrong-category id incorrectly matched\n");
    return ok;
}

bool test_note_range_filter() {
    TestContext ctx_in, ctx_out;
    InputValue value{};

    input_dispatch(ctx_in, kTable, InputCategory::NOTE, 65, value);   // inside 60-72
    input_dispatch(ctx_out, kTable, InputCategory::NOTE, 80, value);  // outside 60-72

    bool ok = ctx_in.calls == 1 && ctx_out.calls == 0;
    printf(ok ? "  OK: note-range filter (id_low/id_high) admits in-range, rejects out-of-range\n"
              : "  FAIL: note-range filter let the wrong note(s) through\n");
    return ok;
}

bool test_channel_filter() {
    TestContext ctx_match, ctx_reject;
    InputValue value_ch5{};
    value_ch5.channel = 5;
    InputValue value_ch6{};
    value_ch6.channel = 6;

    input_dispatch(ctx_match, kTable, InputCategory::MODIFIER, 10, value_ch5);
    input_dispatch(ctx_reject, kTable, InputCategory::MODIFIER, 10, value_ch6);

    bool ok = ctx_match.calls == 1 && ctx_reject.calls == 0;
    printf(ok ? "  OK: channel filter admits the declared channel, rejects others\n"
              : "  FAIL: channel filter let the wrong channel through\n");
    return ok;
}

bool test_fixed_velocity_substitution() {
    TestContext ctx;
    InputValue value{};
    value.velocity = 7;  // should be overridden by the entry's fixed_velocity=100
    input_dispatch(ctx, kTable, InputCategory::NOTE, 90, value);

    bool ok = ctx.calls == 1 && ctx.last_velocity == 100;
    printf(ok ? "  OK: fixed_velocity substitutes the incoming velocity\n"
              : "  FAIL: fixed_velocity did not override incoming velocity\n");
    return ok;
}

bool test_module_global_dispatch() {
    TestContext ctx_ch3, ctx_ch9;
    InputValue value_ch3{};
    value_ch3.channel = 3;
    value_ch3.scalar = 42.0f;
    InputValue value_ch9{};
    value_ch9.channel = 9;
    value_ch9.scalar = 42.0f;

    input_dispatch(ctx_ch3, kTable, InputCategory::MODIFIER, 74, value_ch3);
    input_dispatch(ctx_ch9, kTable, InputCategory::MODIFIER, 74, value_ch9);

    // channel_filter=0xFF: reaches its setter regardless of which channel
    // sent it -- and the setter itself writes a module-global target, not
    // anything per-channel/per-voice. No scope field anywhere in this path.
    bool ok = ctx_ch3.calls == 1 && ctx_ch3.global_target == 42 &&
              ctx_ch9.calls == 1 && ctx_ch9.global_target == 42;
    printf(ok ? "  OK: module-global (FX-style) dispatch writes its target regardless of originating channel\n"
              : "  FAIL: module-global dispatch did not behave the same across channels\n");
    return ok;
}

// --- Compile-time capability check ---

constexpr InputCategory kGoodCapabilities[] = { InputCategory::NOTE, InputCategory::MODIFIER };
constexpr InputCategory kIncompleteCapabilities[] = { InputCategory::NOTE };  // missing MODIFIER

static_assert(input_table_declares_capabilities(kTable, kGoodCapabilities),
              "kTable only uses NOTE/MODIFIER -- this must hold at compile time");
static_assert(!input_table_declares_capabilities(kTable, kIncompleteCapabilities),
              "kTable uses MODIFIER, which kIncompleteCapabilities omits -- this must fail");

bool test_capability_check() {
    // Re-check at runtime too, so a failure here (not just the static_asserts
    // above) shows up in the ALL CHECKS PASSED / CHECKS FAILED summary.
    bool declared = input_table_declares_capabilities(kTable, kGoodCapabilities);
    bool undeclared = input_table_declares_capabilities(kTable, kIncompleteCapabilities);

    bool ok = declared && !undeclared;
    printf(ok ? "  OK: capability check accepts a complete list, rejects an incomplete one\n"
              : "  FAIL: capability check gave the wrong verdict\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== dispatch matches (category, id), calls the right setter ==\n");
    ok = test_dispatch_matches_category_and_id() && ok;
    printf("\n== unmatched id is a no-op ==\n");
    ok = test_no_match_is_noop() && ok;
    printf("\n== matching id under the wrong category is a no-op ==\n");
    ok = test_wrong_category_is_noop() && ok;
    printf("\n== note-range filter (id_low/id_high) ==\n");
    ok = test_note_range_filter() && ok;
    printf("\n== MIDI-channel filter ==\n");
    ok = test_channel_filter() && ok;
    printf("\n== velocity fixed-value substitution ==\n");
    ok = test_fixed_velocity_substitution() && ok;
    printf("\n== module-global (FX-style) Handler dispatch ==\n");
    ok = test_module_global_dispatch() && ok;
    printf("\n== compile-time capability-vs-mapping-table check ==\n");
    ok = test_capability_check() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
