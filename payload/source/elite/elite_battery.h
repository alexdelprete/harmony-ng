#ifndef ELITE_BATTERY_H
#define ELITE_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Battery percentage filter.
 *
 * The cell voltage sags under load: on Alex's 2013 Ultimate it reads about 3.90 V at rest and
 * 3.75 to 3.81 V a few seconds after the screen turns on, 15 to 20 points on the linear
 * 3.40-4.10 V scale. Following every reading made the percentage fall by 2 points every
 * couple of seconds after each wake and jump back at the next one (about 50 reports in
 * 20 minutes). So:
 *   - a rested reading (screen off long enough for the voltage to recover) sets the
 *     percentage, up or down;
 *   - otherwise the percentage never rises and falls by at most one point per
 *     ELITE_BATT_DROP_STEP_MS, which follows a real discharge but not a load sag;
 *   - while charging, the old rules: rise at once, fall by one only when the reading is more
 *     than 4 points below;
 *   - the first reading and a change of the charging state set the percentage directly.
 */

#define ELITE_BATT_MV_EMPTY 3400
#define ELITE_BATT_MV_FULL 4100
#define ELITE_BATT_REST_MS 30000ULL      /* screen off this long = a rested reading */
#define ELITE_BATT_DROP_STEP_MS 120000ULL /* at most one point down per 2 minutes under load */

typedef struct {
    uint8_t pct;          /* 0xFF = no reading yet */
    uint64_t last_drop_ms;
} elite_batt_filter_t;

/* Percentage of a cell voltage, linear over 3.40 to 4.10 V */
int elite_battery_pct_of_mv(int mv);

void elite_battery_filter_init(elite_batt_filter_t *f);

/* One step: avg_mv = the averaged reading; rested = the screen has been off for at least
 * ELITE_BATT_REST_MS. Returns the percentage to report. */
uint8_t elite_battery_filter_step(elite_batt_filter_t *f, int avg_mv, bool charging,
                                  bool chg_transition, bool rested, uint64_t now_ms);

#endif
