#ifndef HUB_MQTT_H
#define HUB_MQTT_H

/*
 * MQTT layer on the event bus (spec milestone 3): retained state topics published on change,
 * inventory chunks, a JSON command channel, no Home Assistant discovery. The transport is
 * mqtt_client.c; the settings endpoint is POST /api/mqtt-save.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char g_mqtt_base_topic[128];

int hub_mqtt_init(void);        /* loads mqtt.json, subscribes to the bus, connects in the background */
void hub_mqtt_tick(void);       /* main loop: client I/O, reconnect backoff, (re)connect actions */
void hub_mqtt_shutdown(void);
void daemon_sync_mqtt(void);    /* settings changed: reload and reconnect */
void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected);
/* Plain publish helper for the system-action "mqtt_publish" (QoS 0, not retained) */
int hub_mqtt_publish_plain(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* HUB_MQTT_H */
