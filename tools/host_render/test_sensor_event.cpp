// Host-buildable unit test for src/sensor_event.h (ticket #103, part of the
// Core 0 Input pipeline redesign, spec #99). Header-only, no hardware
// dependency -- same convention as test_input_layer.cpp / test_envelope.cpp
// / test_voice_alloc.cpp (test_*() functions, an aggregated `bool ok`,
// "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/sensor_event.h"

#include <cstdio>

namespace {

bool test_button_press() {
    SensorEvent ev = sensor_event_button(2, true);
    bool ok = ev.kind == SENSOR_BUTTON && ev.id == 2 && ev.edge == SENSOR_PRESSED;
    printf(ok ? "  OK: sensor_event_button(id, true) produces a SENSOR_BUTTON/SENSOR_PRESSED event\n"
              : "  FAIL: button press event had the wrong kind/id/edge\n");
    return ok;
}

bool test_button_release() {
    SensorEvent ev = sensor_event_button(2, false);
    bool ok = ev.kind == SENSOR_BUTTON && ev.id == 2 && ev.edge == SENSOR_RELEASED;
    printf(ok ? "  OK: sensor_event_button(id, false) produces a SENSOR_BUTTON/SENSOR_RELEASED event\n"
              : "  FAIL: button release event had the wrong kind/id/edge\n");
    return ok;
}

bool test_pot_reading() {
    SensorEvent ev = sensor_event_pot(0, 2048);
    bool ok = ev.kind == SENSOR_POT && ev.id == 0 && ev.raw == 2048;
    printf(ok ? "  OK: sensor_event_pot(id, raw) produces a SENSOR_POT event carrying the raw reading\n"
              : "  FAIL: pot reading event had the wrong kind/id/raw\n");
    return ok;
}

bool test_different_ids_are_independent() {
    SensorEvent a = sensor_event_button(0, true);
    SensorEvent b = sensor_event_button(1, true);
    bool ok = a.id == 0 && b.id == 1 && a.kind == b.kind && a.edge == b.edge;
    printf(ok ? "  OK: two buttons with different ids produce otherwise-identical events distinguished only by id\n"
              : "  FAIL: id was not carried through independently\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== button press ==\n");
    ok = test_button_press() && ok;
    printf("\n== button release ==\n");
    ok = test_button_release() && ok;
    printf("\n== continuous (pot) reading ==\n");
    ok = test_pot_reading() && ok;
    printf("\n== distinct source ids ==\n");
    ok = test_different_ids_are_independent() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
