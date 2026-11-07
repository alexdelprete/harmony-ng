#ifndef ELITE_BATTERY_H
#define ELITE_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Battery percentage filter.
 *
 * Voltage to percentage: a curve through the stock firmware's own points for this remote and
 * PMIC (juniper/init.lua, style.lua, tde/testcharge.lua): shutdown below 3.55 V (0 %), the
 * battery icon steps at 3.70 V and 3.85 V, 3.9 V and up "fully charged", with the flat middle
 * of a Li-ion cell between them. The PMIC reads the cell under the remote's own standby load,
 * so a fresh cell rests near 3.90 V; the old linear 3.40-4.10 V map showed that as 72 %.
 * While charging the reading is about 0.2 V high (testcharge.lua), so it is corrected first.
 *
 * The cell sags under load: about 3.90 V at rest, 3.75 to 3.81 V a few seconds after the
 * screen turns on. Following every reading made the percentage walk down 2 points every couple
 * of seconds after each wake. So:
 *   - a rested reading (screen off long enough for the voltage to recover) sets the percentage,
 *     up or down;
 *   - otherwise, on battery, it never rises and falls by at most one point per
 *     ELITE_BATT_DROP_STEP_MS, which follows a real discharge but not a load sag;
 *   - while charging it rises with the corrected reading and never falls (the charge only goes
 *     up on the cradle);
 *   - the first reading sets it; a change of the charging state keeps the last value (the
 *     reading right after it is taken under the load or the charge current) until a rested or
 *     charging reading moves it.
 *
 * The charging state itself is debounced: lifting the remote off the cradle or setting it down
 * flips it several times within seconds, so a change counts only after it held for
 * ELITE_BATT_CHG_DEBOUNCE_MS.
 */

#define ELITE_BATT_REST_MS 30000ULL         /* screen off this long = a rested reading */
#define ELITE_BATT_DROP_STEP_MS 120000ULL   /* at most one point down per 2 minutes under load */
#define ELITE_BATT_CHG_DEBOUNCE_MS 5000ULL  /* a charging change must hold this long */
#define ELITE_BATT_CHARGE_OFFSET_MV 200     /* the reading is this much high while charging */

typedef struct {
    uint8_t pct;            /* 0xFF = no reading yet */
    uint64_t last_drop_ms;
    /* charging debounce */
    bool chg_known;
    bool chg_stable;        /* the debounced state */
    bool chg_pending;       /* the raw state that differs from chg_stable */
    uint64_t chg_pending_since;
} elite_batt_filter_t;

/* Percentage of a cell voltage read on battery (the curve above) */
int elite_battery_pct_of_mv(int mv);

void elite_battery_filter_init(elite_batt_filter_t *f);

/* Debounced charging state from the raw one; *changed is set when the debounced state changed
 * in this call (never on the first call) */
bool elite_battery_charging(elite_batt_filter_t *f, bool raw, uint64_t now_ms, bool *changed);

/* One step: avg_mv = the averaged reading, charging = the debounced state, chg_transition =
 * it changed in this sample, rested = the screen has been off for ELITE_BATT_REST_MS. Returns
 * the percentage to report. */
uint8_t elite_battery_filter_step(elite_batt_filter_t *f, int avg_mv, bool charging,
                                  bool chg_transition, bool rested, uint64_t now_ms);

#endif
