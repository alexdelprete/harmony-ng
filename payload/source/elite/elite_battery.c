#include "elite_battery.h"

int elite_battery_pct_of_mv(int mv) {
    if (mv >= ELITE_BATT_MV_FULL) return 100;
    if (mv <= ELITE_BATT_MV_EMPTY) return 0;
    return (mv - ELITE_BATT_MV_EMPTY) * 100 / (ELITE_BATT_MV_FULL - ELITE_BATT_MV_EMPTY);
}

void elite_battery_filter_init(elite_batt_filter_t *f) {
    f->pct = 0xFF;
    f->last_drop_ms = 0;
}

uint8_t elite_battery_filter_step(elite_batt_filter_t *f, int avg_mv, bool charging,
                                  bool chg_transition, bool rested, uint64_t now_ms) {
    int pct = elite_battery_pct_of_mv(avg_mv);

    if (f->pct == 0xFF || chg_transition) {
        f->pct = (uint8_t)pct;
        f->last_drop_ms = now_ms;
        return f->pct;
    }

    if (charging) {
        if (pct > f->pct) f->pct = (uint8_t)pct;
        else if (f->pct - pct > 4) f->pct--;
        f->last_drop_ms = now_ms;
        return f->pct;
    }

    if (rested) {
        /* the voltage has recovered: this is the charge left */
        f->pct = (uint8_t)pct;
        f->last_drop_ms = now_ms;
        return f->pct;
    }

    /* under load: never up, and down by one point per step at most */
    if (pct < f->pct) {
        if (now_ms - f->last_drop_ms >= ELITE_BATT_DROP_STEP_MS) {
            f->pct--;
            f->last_drop_ms = now_ms;
        }
    } else {
        f->last_drop_ms = now_ms; /* the step counts from the first reading below */
    }
    return f->pct;
}
