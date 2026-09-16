#pragma once

// Refresh-cadence framework, see CONTEXT.md's Page entry. Every Page
// redraws at a shared 10Hz base rate; a Page opts into a faster 20Hz rate by
// declaring the override on itself -- the framework never reasons about
// *why* a given Page wants it, so a future time-critical Page (tracker's
// playback-position tracking is today's only example) opts in the same way
// any other would. Pure logic, no LCD/timing dependency.

inline constexpr unsigned kRefreshBaseHz = 10;
inline constexpr unsigned kRefreshOverrideHz = 20;

// A Page's own declaration of its refresh-rate needs. Default false: a Page
// gets the 10Hz base rate unless it explicitly opts into the faster one.
struct PageRefreshCadence {
    bool wants_override_rate = false;
};

// The refresh rate a Page should redraw at, given its own declaration.
inline unsigned refresh_rate_hz(const PageRefreshCadence &page) {
    return page.wants_override_rate ? kRefreshOverrideHz : kRefreshBaseHz;
}
