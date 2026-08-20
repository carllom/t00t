// Host-buildable unit test for src/envelope.h's non-interruptible release
// option (ticket #100, part of the Core 0 Input pipeline redesign, spec
// #99). Pure state-machine logic, no audio math beyond what Envelope
// already does and no hardware dependency -- same convention as
// test_input_layer.cpp (test_*() functions, an aggregated `bool ok`,
// "ALL CHECKS PASSED"/"CHECKS FAILED").
//
// Behavior under test, from Envelope's public interface only (state,
// trigger(), release(), advance(), advance_block()) -- never release_pending
// directly, so these tests stay valid across any internal reshuffle:
//
//   - Default config (EnvConfig::gated_attack_decay == true, env_config()'s
//     default): release() still interrupts attack/decay immediately, from
//     any stage -- today's behavior, unchanged.
//   - Non-interruptible config: a release requested during attack or decay
//     never lets the envelope reach ENV_SUSTAIN -- it finishes attack/decay
//     naturally and transitions straight to ENV_RELEASE instead.
//   - Non-interruptible config: a release requested once ENV_SUSTAIN is
//     already reached is immediate (nothing left to protect).
//   - Both advance() (per-sample) and advance_block() (the sub-block API
//     subtractive's audio engine actually calls) are exercised identically,
//     via a shared step function.

#include "../../src/envelope.h"

#include <cstdio>

namespace {

using StepFn = float (*)(Envelope &, const EnvConfig &);

float step_advance(Envelope &e, const EnvConfig &cfg) {
    return e.advance(cfg);
}

// n=1 sub-block: coeff^1 == coeff, so this must behave identically to
// advance() sample-for-sample -- exercises advance_block() itself, per the
// ticket's requirement to verify both APIs.
float step_advance_block(Envelope &e, const EnvConfig &cfg) {
    return e.advance_block(cfg, cfg.decay_coeff, cfg.release_coeff, 1);
}

constexpr int kMaxSteps = 20000;  // generous upper bound; a 2ms stage is ~88 samples at 44100Hz

bool drive_until(Envelope &e, const EnvConfig &cfg, StepFn step, EnvState target, bool &saw_sustain) {
    saw_sustain = false;
    for (int i = 0; i < kMaxSteps; i++) {
        if (e.state == target) return true;
        if (e.state == ENV_IDLE) return false;  // ran out of envelope before reaching target
        if (e.state == ENV_SUSTAIN) saw_sustain = true;
        step(e, cfg);
    }
    return e.state == target;
}

bool test_env_config_defaults_to_gated(const char *) {
    EnvConfig cfg = env_config(10, 100, 70, 800);
    bool ok = cfg.gated_attack_decay == true;
    printf(ok ? "  OK: env_config() defaults gated_attack_decay to true\n"
              : "  FAIL: env_config() did not default gated_attack_decay to true\n");
    return ok;
}

bool test_default_release_interrupts_immediately(const char *step_name, StepFn step) {
    EnvConfig cfg = env_config(2, 2, 50, 2);  // gated_attack_decay left at its default (true)
    Envelope e;
    e.init();
    e.trigger();
    step(e, cfg);  // one sample into attack
    float level_before_release = e.level;
    e.release(cfg);
    bool ok = e.state == ENV_RELEASE && e.level == level_before_release;
    printf(ok ? "  OK [%s]: default (gated) release interrupts attack immediately, level untouched by release() itself\n"
              : "  FAIL [%s]: default release did not interrupt immediately\n", step_name);
    return ok;
}

bool test_release_while_idle_is_noop(const char *step_name, StepFn) {
    EnvConfig cfg_gated = env_config(2, 2, 50, 2);
    EnvConfig cfg_ungated = cfg_gated;
    cfg_ungated.gated_attack_decay = false;

    Envelope e1; e1.init();
    e1.release(cfg_gated);
    bool ok1 = e1.state == ENV_IDLE;

    Envelope e2; e2.init();
    e2.release(cfg_ungated);
    bool ok2 = e2.state == ENV_IDLE;

    bool ok = ok1 && ok2;
    printf(ok ? "  OK [%s]: release() on an idle envelope is a no-op in both gated and non-gated config\n"
              : "  FAIL [%s]: release() on an idle envelope changed state\n", step_name);
    return ok;
}

bool test_non_interruptible_defers_from_attack_start(const char *step_name, StepFn step) {
    EnvConfig cfg = env_config(2, 2, 50, 2);
    cfg.gated_attack_decay = false;

    Envelope e;
    e.init();
    e.trigger();
    e.release(cfg);  // release requested on the very first sample of attack
    bool stayed_attack = (e.state == ENV_ATTACK);

    bool saw_sustain = false;
    bool reached_release = drive_until(e, cfg, step, ENV_RELEASE, saw_sustain);

    bool ok = stayed_attack && !saw_sustain && reached_release;
    printf(ok ? "  OK [%s]: non-interruptible release requested at attack's first sample defers through attack+decay, skips SUSTAIN, then releases\n"
              : "  FAIL [%s]: misbehaved (stayed_attack=%d saw_sustain=%d reached_release=%d)\n",
           step_name, stayed_attack, saw_sustain, reached_release);
    return ok;
}

bool test_non_interruptible_defers_from_mid_decay(const char *step_name, StepFn step) {
    EnvConfig cfg = env_config(2, 2, 50, 2);
    cfg.gated_attack_decay = false;

    Envelope e;
    e.init();
    e.trigger();
    bool ignore;
    bool reached_decay = drive_until(e, cfg, step, ENV_DECAY, ignore);
    // Take a couple of decay steps before requesting release, so this is
    // genuinely mid-decay, not the instant of entry.
    step(e, cfg);
    step(e, cfg);
    bool mid_decay = (e.state == ENV_DECAY);

    e.release(cfg);
    bool stayed_decay = (e.state == ENV_DECAY);

    bool saw_sustain = false;
    bool reached_release = drive_until(e, cfg, step, ENV_RELEASE, saw_sustain);

    bool ok = reached_decay && mid_decay && stayed_decay && !saw_sustain && reached_release;
    printf(ok ? "  OK [%s]: non-interruptible release requested mid-decay defers through decay, skips SUSTAIN, then releases\n"
              : "  FAIL [%s]: misbehaved (reached_decay=%d mid_decay=%d stayed_decay=%d saw_sustain=%d reached_release=%d)\n",
           step_name, reached_decay, mid_decay, stayed_decay, saw_sustain, reached_release);
    return ok;
}

bool test_non_interruptible_release_during_sustain_is_immediate(const char *step_name, StepFn step) {
    EnvConfig cfg = env_config(2, 2, 50, 2);
    cfg.gated_attack_decay = false;

    Envelope e;
    e.init();
    e.trigger();
    bool ignore;
    bool reached_sustain = drive_until(e, cfg, step, ENV_SUSTAIN, ignore);

    e.release(cfg);
    bool ok = reached_sustain && e.state == ENV_RELEASE;
    printf(ok ? "  OK [%s]: non-interruptible release requested once already in SUSTAIN is immediate (nothing left to protect)\n"
              : "  FAIL [%s]: release during SUSTAIN was not immediate (reached_sustain=%d)\n",
           step_name, reached_sustain);
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== env_config() default ==\n");
    ok = test_env_config_defaults_to_gated("n/a") && ok;

    struct { const char *name; StepFn step; } kSteps[] = {
        {"advance", step_advance},
        {"advance_block(n=1)", step_advance_block},
    };

    for (auto &s : kSteps) {
        printf("\n== default (gated) release interrupts immediately [%s] ==\n", s.name);
        ok = test_default_release_interrupts_immediately(s.name, s.step) && ok;

        printf("\n== release() on an idle envelope is a no-op [%s] ==\n", s.name);
        ok = test_release_while_idle_is_noop(s.name, s.step) && ok;

        printf("\n== non-interruptible: release deferred from attack's first sample [%s] ==\n", s.name);
        ok = test_non_interruptible_defers_from_attack_start(s.name, s.step) && ok;

        printf("\n== non-interruptible: release deferred from mid-decay [%s] ==\n", s.name);
        ok = test_non_interruptible_defers_from_mid_decay(s.name, s.step) && ok;

        printf("\n== non-interruptible: release during SUSTAIN is immediate [%s] ==\n", s.name);
        ok = test_non_interruptible_release_during_sustain_is_immediate(s.name, s.step) && ok;
    }

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
