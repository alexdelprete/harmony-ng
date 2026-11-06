/*
 * MQTT v2 (spec section 5): one bus sink publishes activity_state, activity_progress, bt_status,
 * rf_status and button; retained topics are republished on (re)connect from the bus's last
 * payloads, never on a timer; config_changed rebuilds the config and inventory topics; the
 * command channel <base>/cmd mirrors the WebSocket actions through hub_cmd_execute.
 *
 * Topics (<base> = baseTopic, default harmony/hub):
 *   status (LWT online/offline), hub, activity_state, progress, bt, rf, config,
 *   inventory/index, inventory/<deviceId>, remote/<slot>/button (button, no slot: <base>/button),
 *   cmd (in), cmd/response (out), ir/raw (in).
 */
#define _GNU_SOURCE
#include "hub_mqtt.h"
#include "hub_events.h"
#include "hub_cmd.h"
#include "hub_ws.h"
#include "config_store.h"
#include "state_watch.h"
#include "mqtt_client.h"
#include "sha256.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define MQTT_V2_MARKER "/data/harmony/mqtt_v2_migrated"
#define MAX_INVENTORY_DEVICES 64
#define CHUNK_MAX_BYTES 3800   /* under the 4 KB packet buffer with the topic and headers */

/* MQTT configuration */
static char g_mqtt_host[128] = "";
static int  g_mqtt_port = 1883;
char g_mqtt_base_topic[128] = "harmony/hub";
static char g_mqtt_cid[64] = "harmony-local-mqtt";
static char g_mqtt_user[256] = "";
static char g_mqtt_pass[256] = "";
static char g_mqtt_discovery_prefix[64] = "homeassistant"; /* only for clearing the old entities */
static int  g_mqtt_enabled = 0;
static char g_status_topic[160] = "harmony/hub/status";

/* Inventory chunks published so far (to clear removed devices) and their content hashes */
static struct {
    char id[32];
    char hash[17];
} g_inv_pub[MAX_INVENTORY_DEVICES];
static int g_inv_pub_count;
static char g_inventory_version[9] = "";
static char g_last_config_payload[512] = "";

static time_t g_last_mqtt_retry = 0;
static int g_last_mqtt_connected = 0;
static int g_mqtt_retry_interval = 5;
static int g_cleared_old = 0;

/* ------------------------------------------------------------------------------------------ */

void daemon_get_mqtt_info(char *host, size_t hlen, int *port, char *base, size_t blen, int *connected) {
    if (host && hlen > 0) {
        strncpy(host, g_mqtt_enabled ? g_mqtt_host : "", hlen - 1);
        host[hlen - 1] = '\0';
    }
    if (port) *port = g_mqtt_port;
    if (base && blen > 0) {
        strncpy(base, g_mqtt_base_topic, blen - 1);
        base[blen - 1] = '\0';
    }
    if (connected) *connected = mqtt_is_connected();
}

static void set_str(char *dst, size_t cap, const cJSON *j, const char *dflt) {
    if (j && cJSON_IsString(j) && j->valuestring && j->valuestring[0]) snprintf(dst, cap, "%s", j->valuestring);
    else snprintf(dst, cap, "%s", dflt ? dflt : "");
}

/* enabled, host, port, username, password, baseTopic, clientId (and discoveryPrefix, read only
 * to clear the old discovery entities). Returns 1 when MQTT is enabled and has a host. */
static int load_mqtt_config(void) {
    cJSON *root = config_store_get_mqtt_json();
    if (!root) return 0;
    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!en || !cJSON_IsTrue(en) || !h || !cJSON_IsString(h) || !h->valuestring || !h->valuestring[0]) {
        cJSON_Delete(root);
        return 0;
    }
    set_str(g_mqtt_host, sizeof(g_mqtt_host), h, "");
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "port");
    g_mqtt_port = (p && cJSON_IsNumber(p) && p->valueint > 0) ? p->valueint : 1883;
    set_str(g_mqtt_cid, sizeof(g_mqtt_cid), cJSON_GetObjectItemCaseSensitive(root, "clientId"), "harmony-local-mqtt");
    set_str(g_mqtt_user, sizeof(g_mqtt_user), cJSON_GetObjectItemCaseSensitive(root, "username"), "");
    set_str(g_mqtt_pass, sizeof(g_mqtt_pass), cJSON_GetObjectItemCaseSensitive(root, "password"), "");
    set_str(g_mqtt_base_topic, sizeof(g_mqtt_base_topic), cJSON_GetObjectItemCaseSensitive(root, "baseTopic"), "harmony/hub");
    size_t blen = strlen(g_mqtt_base_topic);
    while (blen > 0 && g_mqtt_base_topic[blen - 1] == '/') g_mqtt_base_topic[--blen] = '\0';
    if (!g_mqtt_base_topic[0]) snprintf(g_mqtt_base_topic, sizeof(g_mqtt_base_topic), "harmony/hub");
    set_str(g_mqtt_discovery_prefix, sizeof(g_mqtt_discovery_prefix), cJSON_GetObjectItemCaseSensitive(root, "discoveryPrefix"), "homeassistant");
    snprintf(g_status_topic, sizeof(g_status_topic), "%s/status", g_mqtt_base_topic);
    cJSON_Delete(root);
    return 1;
}

/* ------------------------------------------------------------------------------------------ */
/* Publishing helpers                                                                          */

static int pub_json(const char *suffix, const cJSON *obj, int retain, int qos) {
    if (!mqtt_is_connected() && mqtt_publishing_live()) return -1;
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/%s", g_mqtt_base_topic, suffix);
    char *txt = cJSON_PrintUnformatted(obj);
    if (!txt) return -1;
    int rc = mqtt_publish_ex(topic, txt, strlen(txt), qos, retain);
    free(txt);
    return rc;
}

static void pub_clear(const char *full_topic) {
    mqtt_publish_ex(full_topic, "", 0, 1, 1);
}

int hub_mqtt_publish_plain(const char *topic, const char *payload) {
    if (!g_mqtt_enabled || !topic || !topic[0]) return -1;
    return mqtt_publish_ex(topic, payload ? payload : "", payload ? strlen(payload) : 0, 0, 0);
}

/* ------------------------------------------------------------------------------------------ */
/* Inventory                                                                                   */

static char *read_text(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (len_out) *len_out = n;
    return buf;
}

static void short_hash(const char *txt, size_t len, char out[17], int digits) {
    char hex[65];
    sha256_hex((const uint8_t *)txt, len, hex);
    snprintf(out, 17, "%.*s", digits, hex);
}

static const char *obj_str(const cJSON *o, const char *k) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return (j && cJSON_IsString(j) && j->valuestring) ? j->valuestring : "";
}

static void id_of(const cJSON *d, char *out, size_t cap) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(d, "id");
    if (j && cJSON_IsString(j) && j->valuestring) snprintf(out, cap, "%s", j->valuestring);
    else if (j && cJSON_IsNumber(j)) snprintf(out, cap, "%ld", (long)j->valuedouble);
    else out[0] = '\0';
}

static int inv_pub_find(const char *id) {
    for (int i = 0; i < g_inv_pub_count; i++) if (strcmp(g_inv_pub[i].id, id) == 0) return i;
    return -1;
}

/* Publishes one device as inventory/<id> (split into parts when it does not fit the packet) */
static void publish_device_chunk(const char *id, cJSON *chunk) {
    char suffix[80];
    char *txt = cJSON_PrintUnformatted(chunk);
    if (!txt) return;
    size_t len = strlen(txt);
    if (len <= CHUNK_MAX_BYTES) {
        snprintf(suffix, sizeof(suffix), "inventory/%s", id);
        pub_json(suffix, chunk, 1, 1);
        free(txt);
        return;
    }
    free(txt);
    /* Split the command names over several parts */
    cJSON *cmds = cJSON_DetachItemFromObjectCaseSensitive(chunk, "commands");
    int ncmd = cmds ? cJSON_GetArraySize(cmds) : 0;
    int per_part = 120;
    int parts = (ncmd + per_part - 1) / per_part;
    if (parts < 1) parts = 1;
    for (int part = 1; part <= parts; part++) {
        cJSON *pc = cJSON_Duplicate(chunk, 1);
        cJSON *arr = cJSON_AddArrayToObject(pc, "commands");
        for (int i = (part - 1) * per_part; i < part * per_part && i < ncmd; i++) {
            cJSON_AddItemToArray(arr, cJSON_Duplicate(cJSON_GetArrayItem(cmds, i), 1));
        }
        cJSON_AddNumberToObject(pc, "part", part);
        cJSON_AddNumberToObject(pc, "parts", parts);
        snprintf(suffix, sizeof(suffix), "inventory/%s/%d", id, part);
        pub_json(suffix, pc, 1, 1);
        cJSON_Delete(pc);
    }
    cJSON_Delete(cmds);
}

/* Rebuilds config and inventory. force: republish every chunk (a reconnect). */
static void publish_inventory(int force) {
    size_t dlen = 0, alen = 0;
    char *dtxt = read_text(HARMONY_DEVICES_FILE, &dlen);
    char *atxt = read_text(HARMONY_ACTIVITIES_FILE, &alen);
    /* inventory_version: first 8 hex of SHA-256 over both files */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (dtxt) sha256_update(&ctx, (const uint8_t *)dtxt, dlen);
    if (atxt) sha256_update(&ctx, (const uint8_t *)atxt, alen);
    uint8_t digest[32];
    sha256_final(digest, &ctx);
    char version[9];
    snprintf(version, sizeof(version), "%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
    int version_changed = strcmp(version, g_inventory_version) != 0;
    snprintf(g_inventory_version, sizeof(g_inventory_version), "%s", version);

    cJSON *droot = dtxt ? cJSON_Parse(dtxt) : NULL;
    cJSON *aroot = atxt ? cJSON_Parse(atxt) : NULL;
    free(dtxt);
    free(atxt);
    cJSON *devs = droot ? (cJSON_IsArray(droot) ? droot : cJSON_GetObjectItemCaseSensitive(droot, "devices")) : NULL;
    cJSON *acts = aroot ? (cJSON_IsArray(aroot) ? aroot : cJSON_GetObjectItemCaseSensitive(aroot, "activities")) : NULL;

    /* Device chunks: republished only when their own content changed */
    char seen[MAX_INVENTORY_DEVICES][32];
    int nseen = 0;
    cJSON *index = cJSON_CreateObject();
    cJSON_AddStringToObject(index, "type", "inventory_index");
    cJSON_AddStringToObject(index, "inventory_version", version);
    cJSON *idx_devs = cJSON_AddArrayToObject(index, "devices");
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        char id[32];
        id_of(d, id, sizeof(id));
        if (!id[0] || nseen >= MAX_INVENTORY_DEVICES) continue;
        snprintf(seen[nseen++], 32, "%s", id);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", id);
        cJSON_AddStringToObject(entry, "name", obj_str(d, "name"));
        cJSON_AddStringToObject(entry, "deviceType", obj_str(d, "type"));
        cJSON_AddStringToObject(entry, "manufacturer", obj_str(d, "manufacturer"));
        cJSON_AddStringToObject(entry, "model", obj_str(d, "model"));
        cJSON_AddItemToArray(idx_devs, entry);

        /* the device's own "type" (Television, ...) goes under deviceType: "type" is the
         * message type on every topic */
        cJSON *chunk = cJSON_CreateObject();
        cJSON_AddStringToObject(chunk, "type", "inventory_device");
        cJSON_AddStringToObject(chunk, "id", id);
        cJSON_AddStringToObject(chunk, "name", obj_str(d, "name"));
        cJSON_AddStringToObject(chunk, "deviceType", obj_str(d, "type"));
        cJSON_AddStringToObject(chunk, "manufacturer", obj_str(d, "manufacturer"));
        cJSON_AddStringToObject(chunk, "model", obj_str(d, "model"));
        const cJSON *cp = cJSON_GetObjectItemCaseSensitive(d, "controlPort");
        cJSON_AddNumberToObject(chunk, "controlPort", (cp && cJSON_IsNumber(cp)) ? cp->valueint : 0);
        cJSON *cmds = cJSON_AddArrayToObject(chunk, "commands");
        const cJSON *c = NULL;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(d, "commands")) {
            const char *nm = cJSON_IsString(c) ? c->valuestring : obj_str(c, "name");
            if (nm && nm[0]) cJSON_AddItemToArray(cmds, cJSON_CreateString(nm));
        }
        char *txt = cJSON_PrintUnformatted(chunk);
        char h[17] = "";
        if (txt) short_hash(txt, strlen(txt), h, 16);
        free(txt);
        int k = inv_pub_find(id);
        if (force || k < 0 || strcmp(g_inv_pub[k].hash, h) != 0) {
            publish_device_chunk(id, chunk);
            if (k < 0 && g_inv_pub_count < MAX_INVENTORY_DEVICES) {
                k = g_inv_pub_count++;
                snprintf(g_inv_pub[k].id, sizeof(g_inv_pub[k].id), "%s", id);
            }
            if (k >= 0) snprintf(g_inv_pub[k].hash, sizeof(g_inv_pub[k].hash), "%s", h);
        }
        cJSON_Delete(chunk);
    }
    /* Removed devices: clear their retained chunk */
    for (int i = 0; i < g_inv_pub_count; i++) {
        int still = 0;
        for (int j = 0; j < nseen; j++) if (strcmp(seen[j], g_inv_pub[i].id) == 0) still = 1;
        if (!still) {
            char topic[256];
            snprintf(topic, sizeof(topic), "%s/inventory/%s", g_mqtt_base_topic, g_inv_pub[i].id);
            pub_clear(topic);
            g_inv_pub[i] = g_inv_pub[g_inv_pub_count - 1];
            g_inv_pub_count--;
            i--;
        }
    }
    /* index last, so a consumer that waits for it sees complete chunks */
    cJSON *idx_acts = cJSON_AddArrayToObject(index, "activities");
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        char id[32];
        id_of(a, id, sizeof(id));
        if (!id[0]) continue;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", id);
        cJSON_AddStringToObject(entry, "name", obj_str(a, "name"));
        cJSON_AddStringToObject(entry, "activityType", obj_str(a, "type"));
        const cJSON *ord = cJSON_GetObjectItemCaseSensitive(a, "order");
        cJSON_AddNumberToObject(entry, "order", (ord && cJSON_IsNumber(ord)) ? ord->valueint : 0);
        cJSON_AddItemToArray(idx_acts, entry);
    }
    if (force || version_changed) pub_json("inventory/index", index, 1, 1);
    cJSON_Delete(index);
    cJSON_Delete(droot);
    cJSON_Delete(aroot);

    /* config: the watcher's mtimes plus the inventory version */
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "type", "config");
    cJSON *files = state_watch_config_mtimes();
    if (files) cJSON_AddItemToObject(cfg, "files", files);
    cJSON_AddStringToObject(cfg, "inventory_version", version);
    cJSON_AddNumberToObject(cfg, "ts", (double)time(NULL));
    char *ctxt = cJSON_PrintUnformatted(cfg);
    if (ctxt) {
        /* the ts changes every time: compare without it */
        char *cut = strstr(ctxt, ",\"ts\":");
        size_t cmp_len = cut ? (size_t)(cut - ctxt) : strlen(ctxt);
        if (force || strncmp(ctxt, g_last_config_payload, cmp_len) != 0 || g_last_config_payload[cmp_len] != '\0') {
            pub_json("config", cfg, 1, 1);
            snprintf(g_last_config_payload, sizeof(g_last_config_payload), "%.*s", (int)cmp_len, ctxt);
        }
        free(ctxt);
    }
    cJSON_Delete(cfg);
}

/* ------------------------------------------------------------------------------------------ */
/* Old topics and discovery entities of the previous MQTT layer: cleared once                   */

static void safe_id(const char *in, char *out, size_t outlen) {
    size_t i = 0;
    while (in && *in && i < outlen - 1) {
        char c = *in++;
        out[i++] = (isalnum((unsigned char)c) || c == '-' || c == '_') ? (char)tolower((unsigned char)c) : '_';
    }
    out[i] = '\0';
}

static void clear_old_topics(void) {
    if (g_cleared_old) return;
    g_cleared_old = 1;
    if (access(MQTT_V2_MARKER, F_OK) == 0) return;
    static const char *const old_state[] = { "state", "activity", "current_activity", "activity_id", "activity/status", NULL };
    char topic[256];
    for (int i = 0; old_state[i]; i++) {
        snprintf(topic, sizeof(topic), "%s/%s", g_mqtt_base_topic, old_state[i]);
        pub_clear(topic);
    }
    /* Home Assistant discovery configs the old hub_mqtt_ha.c published */
    char ident[64];
    safe_id(g_mqtt_cid, ident, sizeof(ident));
    static const char *const disc[] = {
        "select/%s_activity/config", "sensor/%s_activity_id/config", "sensor/%s_ip/config",
        "sensor/%s_firmware/config", "sensor/%s_ir_devices/config", "sensor/%s_ir_commands/config",
        "button/%s_power_off/config", "button/%s_sync/config", "button/%s_remote_1_sleep/config",
        "button/%s_remote_2_sleep/config", NULL
    };
    for (int i = 0; disc[i]; i++) {
        char obj[128];
        snprintf(obj, sizeof(obj), disc[i], ident);
        snprintf(topic, sizeof(topic), "%s/%s", g_mqtt_discovery_prefix, obj);
        pub_clear(topic);
    }
    printf("[*] MQTT: cleared the old state topics and %d discovery entities (%s/*/%s_*)\n", 10, g_mqtt_discovery_prefix, ident);
    FILE *f = fopen(MQTT_V2_MARKER, "w");
    if (f) {
        fprintf(f, "%ld\n", (long)time(NULL));
        fclose(f);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Command channel                                                                             */

static void publish_cmd_response(cJSON *resp) {
    if (!resp) return;
    pub_json("cmd/response", resp, 0, 1);
}

/* Recovers "id":"..." from a payload that failed to parse */
static int recover_id(const char *payload, char *out, size_t cap) {
    const char *p = payload ? strstr(payload, "\"id\"") : NULL;
    if (!p) return 0;
    p = strchr(p + 4, '"');
    if (!p) return 0;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap && n < 64) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static void on_cmd(const char *topic, const char *payload, size_t len, void *ud) {
    (void)ud;
    (void)len;
    int is_ir_raw = strstr(topic, "/ir/raw") != NULL;
    cJSON *req = (payload && payload[0]) ? cJSON_Parse(payload) : NULL;
    if (!req || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        char id[65];
        if (recover_id(payload, id, sizeof(id))) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "type", "cmd_response");
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddStringToObject(resp, "action", is_ir_raw ? "ir_raw" : "");
            cJSON_AddBoolToObject(resp, "ok", 0);
            cJSON_AddNumberToObject(resp, "code", 400);
            cJSON_AddStringToObject(resp, "error", "malformed JSON");
            publish_cmd_response(resp);
            cJSON_Delete(resp);
        } else {
            printf("[!] MQTT: malformed request on %s\n", topic);
        }
        return;
    }
    if (is_ir_raw) {
        cJSON_DeleteItemFromObjectCaseSensitive(req, "action");
        cJSON_AddStringToObject(req, "action", "ir_raw");
    }
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");
    int has_id = jid && cJSON_IsString(jid) && jid->valuestring[0];
    cJSON *reply = hub_cmd_execute(req, 2 /* ROLE_ADMIN: the broker is trusted */);
    if (reply && has_id) {
        const char *rtype = obj_str(reply, "type");
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "cmd_response");
        cJSON_AddStringToObject(resp, "id", jid->valuestring);
        cJSON_AddStringToObject(resp, "action", obj_str(req, "action"));
        if (strcmp(rtype, "ack") == 0) {
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(reply, "ok");
            int okv = ok && cJSON_IsTrue(ok);
            cJSON_AddBoolToObject(resp, "ok", okv);
            if (!okv) {
                const cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
                cJSON_AddNumberToObject(resp, "code", code && cJSON_IsNumber(code) ? code->valueint : 500);
                cJSON_AddStringToObject(resp, "error", obj_str(reply, "error"));
            }
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(reply, "count");
            const cJSON *dm = cJSON_GetObjectItemCaseSensitive(reply, "duration_ms");
            if (c) cJSON_AddNumberToObject(resp, "count", c->valuedouble);
            if (dm) cJSON_AddNumberToObject(resp, "duration_ms", dm->valuedouble);
        } else {
            /* pong, activity_state, snapshot: the object itself under its type name */
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_DeleteItemFromObjectCaseSensitive(reply, "id");
            cJSON_AddItemToObject(resp, strcmp(rtype, "pong") == 0 ? "pong" : rtype, cJSON_Duplicate(reply, 1));
        }
        publish_cmd_response(resp);
        cJSON_Delete(resp);
    }
    cJSON_Delete(reply);
    cJSON_Delete(req);
}

static void subscribe_cmd_topics(void) {
    char t[256];
    snprintf(t, sizeof(t), "%s/cmd", g_mqtt_base_topic);
    mqtt_subscribe(t, on_cmd, NULL);
    snprintf(t, sizeof(t), "%s/ir/raw", g_mqtt_base_topic);
    mqtt_subscribe(t, on_cmd, NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* Bus sink and (re)connect                                                                    */

static void publish_hub_object(void) {
    cJSON *snap = hub_ws_snapshot_object(0);
    if (!snap) return;
    cJSON *hub = cJSON_DetachItemFromObjectCaseSensitive(snap, "hub");
    cJSON_Delete(snap);
    if (!hub) return;
    cJSON_DeleteItemFromObjectCaseSensitive(hub, "type");
    cJSON *typed = cJSON_CreateObject();
    cJSON_AddStringToObject(typed, "type", "hub");
    cJSON *child;
    while ((child = hub->child) != NULL) {
        cJSON_DetachItemViaPointer(hub, child);
        cJSON_AddItemToObject(typed, child->string, child);
    }
    cJSON_Delete(hub);
    cJSON_AddNumberToObject(typed, "ts", (double)time(NULL));
    pub_json("hub", typed, 1, 1);
    cJSON_Delete(typed);
}

static void publish_last(hub_event_type_t type, const char *suffix) {
    const cJSON *last = hub_events_last(type);
    if (!last) return;
    pub_json(suffix, last, 1, 1);
}

static void on_connected(void) {
    subscribe_cmd_topics();
    clear_old_topics();
    publish_hub_object();
    publish_last(HUB_EV_ACTIVITY_STATE, "activity_state");
    publish_last(HUB_EV_BT_STATUS, "bt");
    publish_last(HUB_EV_RF_STATUS, "rf");
    publish_inventory(1);
}

static void mqtt_event_sink(const hub_event_t *ev, void *user) {
    (void)user;
    if (!g_mqtt_enabled || !ev || !ev->payload) return;
    if (!mqtt_is_connected() && mqtt_publishing_live()) return; /* dropped while the broker is down */
    switch (ev->type) {
        case HUB_EV_ACTIVITY_STATE: pub_json("activity_state", ev->payload, 1, 1); break;
        case HUB_EV_ACTIVITY_PROGRESS: pub_json("progress", ev->payload, 0, 0); break;
        case HUB_EV_BT_STATUS: pub_json("bt", ev->payload, 1, 1); break;
        case HUB_EV_RF_STATUS: pub_json("rf", ev->payload, 1, 1); break;
        case HUB_EV_BUTTON: {
            const cJSON *slot = cJSON_GetObjectItemCaseSensitive(ev->payload, "slot");
            char suffix[48];
            if (slot && cJSON_IsNumber(slot)) snprintf(suffix, sizeof(suffix), "remote/%d/button", slot->valueint);
            else snprintf(suffix, sizeof(suffix), "button");
            pub_json(suffix, ev->payload, 0, 1);
            break;
        }
        case HUB_EV_CONFIG_CHANGED:
            publish_inventory(0);
            publish_hub_object(); /* hub_name may have changed */
            break;
        default: break;
    }
}

int hub_mqtt_init(void) {
    hub_events_subscribe(mqtt_event_sink, NULL);
    g_mqtt_enabled = load_mqtt_config();
    if (!g_mqtt_enabled) {
        printf("[*] MQTT disabled or config missing\n");
        return 0;
    }
    printf("[+] MQTT config loaded: %s:%d (user=%s, client=%s, base=%s)\n",
           g_mqtt_host, g_mqtt_port, g_mqtt_user[0] ? g_mqtt_user : "none", g_mqtt_cid, g_mqtt_base_topic);
    mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    mqtt_set_lwt(g_status_topic, "offline", 1);
    mqtt_set_birth(g_status_topic, "online", 1);
    printf("[*] Connecting to MQTT broker %s:%d in background...\n", g_mqtt_host, g_mqtt_port);
    mqtt_connect_async();
    return 1;
}

void daemon_sync_mqtt(void) {
    int was_enabled = g_mqtt_enabled;
    g_mqtt_enabled = load_mqtt_config();
    if (mqtt_is_connected() || was_enabled) {
        mqtt_disconnect();
        g_last_mqtt_connected = 0;
    }
    g_inv_pub_count = 0;
    g_inventory_version[0] = '\0';
    g_last_config_payload[0] = '\0';
    if (!g_mqtt_enabled) return;
    mqtt_init(g_mqtt_host, g_mqtt_port, g_mqtt_cid, g_mqtt_user, g_mqtt_pass);
    mqtt_set_lwt(g_status_topic, "offline", 1);
    mqtt_set_birth(g_status_topic, "online", 1);
    g_mqtt_retry_interval = 5;
    g_last_mqtt_retry = 0;
    mqtt_connect_async();
}

void hub_mqtt_tick(void) {
    if (!g_mqtt_enabled) return;
    if (mqtt_is_connected()) {
        if (!g_last_mqtt_connected) {
            g_last_mqtt_connected = 1;
            g_mqtt_retry_interval = 5;
            printf("[+] Connected to MQTT broker %s:%d\n", g_mqtt_host, g_mqtt_port);
            on_connected();
        }
        mqtt_tick();
    } else {
        if (g_last_mqtt_connected) {
            g_last_mqtt_connected = 0;
            printf("[-] MQTT connection lost, will reconnect with backoff...\n");
        }
        time_t now = time(NULL);
        if (now - g_last_mqtt_retry >= g_mqtt_retry_interval) {
            g_last_mqtt_retry = now;
            if (!mqtt_is_connecting()) {
                mqtt_connect_async();
                g_mqtt_retry_interval = (g_mqtt_retry_interval * 2 > 60) ? 60 : (g_mqtt_retry_interval * 2);
            }
        }
    }
}

void hub_mqtt_shutdown(void) {
    if (g_mqtt_enabled) mqtt_disconnect();
}
