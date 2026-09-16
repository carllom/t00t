// Host-buildable unit test for src/wslcd/refresh_cadence.h: the shared
// refresh-cadence framework. Pure logic, no LCD stub dependency -- plain
// no-arg invocation (test_*() functions, an aggregated `bool ok`, "ALL
// CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/refresh_cadence.h"

#include <cstdio>

namespace {

bool test_override_flag_selects_20hz() {
    PageRefreshCadence page;
    page.wants_override_rate = true;

    bool ok = refresh_rate_hz(page) == 20;
    printf(ok ? "  OK: a Page declaring the override rate gets 20Hz\n"
              : "  FAIL: an override-declaring Page did not get 20Hz\n");
    return ok;
}

bool test_no_override_flag_selects_10hz_base_rate() {
    PageRefreshCadence page;  // wants_override_rate defaults to false

    bool ok = refresh_rate_hz(page) == 10;
    printf(ok ? "  OK: a Page with no override declared gets the 10Hz base rate\n"
              : "  FAIL: a non-overriding Page did not get the 10Hz base rate\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== override flag selects 20Hz vs. the 10Hz base rate ==\n");
    ok = test_override_flag_selects_20hz() && ok;
    ok = test_no_override_flag_selects_10hz_base_rate() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
