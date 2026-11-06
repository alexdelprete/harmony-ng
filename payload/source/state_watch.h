#ifndef HARMONY_STATE_WATCH_H
#define HARMONY_STATE_WATCH_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the thread that emits bt_status, rf_status and config_changed events on the bus
 * when that state changes. Returns 0 on success. */
int state_watch_start(void);

/* Offer an RF status object (the GET /api/rf/status object, ownership passes): emits rf_status
 * when it differs from the last one (link counters and battery voltage ignored). Any thread.
 * Returns 1 when an event was emitted. */
int state_watch_offer_rf_status(cJSON *obj);

/* New object {"devices": mtime, "activities": mtime, "settings": mtime, "remote_map": mtime}
 * (Unix seconds, 0 when a file is missing; remote_map = the newest slot map) for the snapshot */
cJSON *state_watch_config_mtimes(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_STATE_WATCH_H */
