/* Battery percentage filter (payload/source/elite/elite_battery.c).
 * Replays the pattern a 2013 Ultimate showed on 2026-11-07: about 3.90 V at rest, a sag to
 * 3.81 V within seconds of the screen turning on, back at rest after the blank. */
#include <stdio.h>
#include <stdlib.h>
#include "elite_battery.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

/* The report rule of elite_main.c: every 5 minutes, on a charging change, or with the screen
 * on for a change of 2 points or more, at most once a minute */
typedef struct { uint64_t last_ms; int last_pct; int count; } reporter_t;
static void report_tick(reporter_t *r, int pct, bool screen_on, uint64_t now, bool limit_minute) {
    bool interval = (r->count == 0 || now - r->last_ms >= 300000);
    bool changed = (r->last_pct < 0 || abs(pct - r->last_pct) >= 2);
    bool due = screen_on && changed && (!limit_minute || now - r->last_ms >= 60000);
    if (interval || due) { r->last_ms = now; r->last_pct = pct; r->count++; }
}

/* The filter before this change: follows every drop at once, jumps up after a rise of 5 */
static int old_filter(int *f, int mv) {
    int pct = elite_battery_pct_of_mv(mv);
    if (*f < 0) *f = pct;
    else if (pct < *f) *f = pct;
    else if (pct - *f > 4) *f = pct;
    return *f;
}

/* One wake: 40 s screen on (the voltage walks from rest to the sag over 16 s, like the
 * 16-sample average), then 120 s off. Returns the lowest and highest percentage seen. */
static void wake_cycle(elite_batt_filter_t *f, int *oldf, reporter_t *rn, reporter_t *ro, uint64_t *t,
                       int rest_mv, int sag_mv, int *lo, int *hi) {
    *lo = 101; *hi = -1;
    for (int s = 0; s < 160; s++, *t += 1000) {
        bool screen = s < 40;
        int k = screen ? s : (s - 40);
        int mv = screen ? (k >= 16 ? sag_mv : rest_mv - (rest_mv - sag_mv) * k / 16)
                        : (k >= 16 ? rest_mv : sag_mv + (rest_mv - sag_mv) * k / 16);
        bool rested = !screen && (s - 40) * 1000ULL >= ELITE_BATT_REST_MS;
        int p = elite_battery_filter_step(f, mv, false, false, rested, *t);
        int po = old_filter(oldf, mv);
        report_tick(rn, p, screen, *t, true);
        report_tick(ro, po, screen, *t, false);
        if (p < *lo) *lo = p;
        if (p > *hi) *hi = p;
    }
}

int main(void) {
    printf("[*] Battery filter tests...\n");
    CHECK(elite_battery_pct_of_mv(3400) == 0, "3.40 V = 0 %");
    CHECK(elite_battery_pct_of_mv(4100) == 100, "4.10 V = 100 %");
    CHECK(elite_battery_pct_of_mv(3750) == 50, "3.75 V = 50 %");

    /* 1. seven wakes in 20 minutes, rested 3.908 V (72 %), sagging to 3.810 V (58 %) */
    elite_batt_filter_t f; elite_battery_filter_init(&f);
    int oldf = -1;
    reporter_t rn = {0, -1, 0}, ro = {0, -1, 0};
    uint64_t t = 1000;
    elite_battery_filter_step(&f, 3908, false, false, true, t);
    int lo = 0, hi = 0, worst_lo = 101;
    for (int i = 0; i < 7; i++) {
        wake_cycle(&f, &oldf, &rn, &ro, &t, 3908 - i * 2, 3810 - i * 2, &lo, &hi);
        if (lo < worst_lo) worst_lo = lo;
    }
    printf("    7 wakes: lowest %d %%, reports new %d, old %d\n", worst_lo, rn.count, ro.count);
    CHECK(worst_lo >= 70, "a 40 s wake moves the percentage by at most one point");
    CHECK(rn.count <= 5, "about one report per 5 minutes, not one per sag step");
    CHECK(ro.count >= 30, "the old rule reported the sag (sanity check of the replay)");

    /* 2. a long session: 10 minutes at the sag voltage drops at most 5 points */
    elite_battery_filter_init(&f);
    t = 0;
    elite_battery_filter_step(&f, 3908, false, false, true, t);
    int p = 0;
    for (int s = 1; s <= 600; s++) p = elite_battery_filter_step(&f, 3810, false, false, false, t + s * 1000ULL);
    CHECK(p >= 67 && p <= 71, "10 minutes under load: at most 5 points down");
    /* back at rest: the rested voltage sets the percentage again, up included */
    p = elite_battery_filter_step(&f, 3900, false, false, true, t + 700000);
    CHECK(p == elite_battery_pct_of_mv(3900), "a rested reading sets the percentage, up or down");
    p = elite_battery_filter_step(&f, 3880, false, false, true, t + 701000);
    CHECK(p == elite_battery_pct_of_mv(3880), "rested readings follow a real discharge at once");

    /* 3. under load the percentage never rises */
    p = elite_battery_filter_step(&f, 4000, false, false, false, t + 702000);
    CHECK(p == elite_battery_pct_of_mv(3880), "no rise under load");

    /* 4. charging: rise at once, small dips ignored; a charging change resets */
    elite_battery_filter_init(&f);
    p = elite_battery_filter_step(&f, 3800, true, false, false, 0);
    CHECK(p == elite_battery_pct_of_mv(3800), "first reading sets it");
    p = elite_battery_filter_step(&f, 3900, true, false, false, 1000);
    CHECK(p == elite_battery_pct_of_mv(3900), "charging: rises at once");
    p = elite_battery_filter_step(&f, 3880, true, false, false, 2000);
    CHECK(p == elite_battery_pct_of_mv(3900), "charging: a small dip is ignored");
    p = elite_battery_filter_step(&f, 3700, false, true, false, 3000);
    CHECK(p == elite_battery_pct_of_mv(3700), "a charging change sets it directly");

    if (g_failures) { printf("[-] %d battery filter checks failed\n", g_failures); return 1; }
    printf("[+] All battery filter checks passed\n");
    return 0;
}
