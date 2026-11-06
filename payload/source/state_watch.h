#ifndef HARMONY_STATE_WATCH_H
#define HARMONY_STATE_WATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the thread that emits bt_status, rf_status and config_changed events on the bus
 * when that state changes. Returns 0 on success. */
int state_watch_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_STATE_WATCH_H */
