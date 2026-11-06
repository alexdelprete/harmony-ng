#!/usr/bin/env python3
"""
MQTT v2 (spec section 5) against hub_daemon_test connected to the stdlib mock broker
(tests/mock_mqtt.py), publishing for real through HARMONY_MQTT_LIVE=1: retained topics after
connect, the one-time clearing of the old state and discovery topics, activity_state and progress
on a dry-run switch, bt/rf retained from pushes, inventory chunks and inventory_version, the
command channel (cmd_response with the same id, 409 busy, get_snapshot, malformed JSON, no id = no
reply) and ir/raw reaching the recorded encoder call.
"""

import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402
from mock_mqtt import MockBroker, MockClient  # noqa: E402

PORT = 8989
BASE = "harmony/hub"
UNIT_ID = "20140101120000-000001"
BT_STATUS_FILE = "/tmp/hub_btstack_status.json"
failed = 0


def check(cond, name, detail=""):
    global failed
    if cond:
        print("  PASS %s" % name)
    else:
        failed += 1
        print("  FAIL %s %s" % (name, detail))


def rec(s):
    b = s.encode("ascii") if isinstance(s, str) else s
    return struct.pack("!I", len(b)) + b


def mfg_blob(unit_id):
    recs = b"harmony!" + rec("unitid") + rec(unit_id) + rec("arch") + rec("0x11")
    img = bytearray(b"\xff" * 0x10000)
    img[0x9000:0x9000 + len(recs)] = recs
    return bytes(img)


def retained_json(broker, topic):
    p = broker.retained.get(topic)
    return json.loads(p.decode("utf-8")) if p else None


def jload(payload):
    try:
        return json.loads(payload.decode("utf-8"))
    except ValueError:
        return None


def main():
    import tempfile
    broker = MockBroker()
    with tempfile.NamedTemporaryFile(prefix="mfg_", suffix=".bin", delete=False) as f:
        f.write(mfg_blob(UNIT_ID))
        mfg_path = f.name
    mqtt_cfg = {"enabled": True, "host": "127.0.0.1", "port": broker.port, "baseTopic": BASE,
                "clientId": "harmony-test", "discoveryPrefix": "homeassistant",
                "slot1RemoteEvents": True, "haDiscovery": True}
    try:
        print("[-] 5.1/5.2 retained topics after connect, old topics cleared")
        with HubHarness(port=PORT, args=["--mfg-file", mfg_path], env={"HARMONY_MQTT_LIVE": "1"},
                        extra_data_files={"mqtt.json": mqtt_cfg}) as hub:
            sub = MockClient("127.0.0.1", broker.port, "observer")
            sub.subscribe(BASE + "/#")
            sub.subscribe("homeassistant/#")
            # wait for the daemon to connect and publish
            deadline = time.time() + 10
            while time.time() < deadline and (BASE + "/inventory/index") not in broker.retained:
                time.sleep(0.1)
            r = broker.retained
            check(r.get(BASE + "/status") == b"online", "status online retained", str(r.get(BASE + "/status")))
            hubo = retained_json(broker, BASE + "/hub")
            check(hubo is not None and hubo.get("type") == "hub" and hubo.get("hub_id") == UNIT_ID
                  and "mqtt_v2" in hubo.get("capabilities", []) and "ts" in hubo, "hub retained with hub_id and capabilities", str(hubo))
            act = retained_json(broker, BASE + "/activity_state")
            check(act is not None and act.get("type") == "activity_state" and act.get("activity") == "-1", "activity_state retained (idle)", str(act))
            cfg = retained_json(broker, BASE + "/config")
            check(cfg is not None and cfg.get("type") == "config" and len(cfg.get("inventory_version", "")) == 8
                  and set(cfg.get("files", {}).keys()) >= {"devices", "activities", "settings", "remote_map"},
                  "config retained with mtimes and inventory_version", str(cfg))
            idx = retained_json(broker, BASE + "/inventory/index")
            check(idx is not None and idx.get("type") == "inventory_index" and idx.get("inventory_version") == cfg.get("inventory_version")
                  and [d["id"] for d in idx.get("devices", [])] == ["1", "2"] and [a["id"] for a in idx.get("activities", [])] == ["10", "11"],
                  "inventory/index lists the sample devices and activities", str(idx))
            d1 = retained_json(broker, BASE + "/inventory/1")
            check(d1 is not None and d1.get("type") == "inventory_device" and d1.get("name") == "Living Room TV"
                  and d1.get("commands") == ["PowerOn", "PowerOff", "InputHdmi1", "VolumeUp", "VolumeDown"]
                  and "raw" not in json.dumps(d1), "inventory/1: names only, no codes", str(d1))
            check(d1 is not None and d1.get("deviceType") == "Television" and d1.get("controlPort") == 7, "device chunk fields", str(d1))
            # old topics cleared once: empty retained publishes in the broker log
            cleared = {t for (t, p, rt, q) in broker.log if rt and p == b""}
            for t in ("state", "activity", "current_activity", "activity_id"):
                check((BASE + "/" + t) in cleared, "old topic %s cleared" % t, str(sorted(cleared))[:200])
            for t in ("select/harmony-test_activity/config", "sensor/harmony-test_activity_id/config",
                      "button/harmony-test_power_off/config", "button/harmony-test_remote_2_sleep/config"):
                check(("homeassistant/" + t) in cleared, "discovery %s cleared" % t.split("/")[1])
            marker = os.path.join(hub.data_dir, "mqtt_v2_migrated")
            check(os.path.exists(marker), "marker file written once")
            # nothing old is published or subscribed any more
            published = {t for (t, p, rt, q) in broker.log if p}
            check(not any(t.endswith("/state") or t.endswith("/activity_id") for t in published), "no old state payloads", str(published))

            print("[-] 5.2 events: activity switch, bt and rf pushes")
            sub.drain()
            st, _ = hub.http_post("/api/activity", {"activity": "10"})
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 2)
            check(m is not None and jload(m[1]).get("activity") == "10", "activity_state running published", str(m))
            prog = [x for x in sub.drain() if x[0] == BASE + "/progress"]
            check(len(prog) >= 2 and all(not x[2] for x in prog) and jload(prog[0][1]).get("type") == "activity_progress",
                  "progress frames, not retained (%d)" % len(prog), str(prog[:1]))
            check(retained_json(broker, BASE + "/activity_state").get("activity") == "10", "activity_state retained value follows")
            hub.http_post("/api/poweroff", {})
            sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 0)
            # rf push and bt file
            rf = {"running": True, "ok": True, "slots": [{"slot": 1, "active": True, "battery": {"percent": 50, "charging": False, "voltage_mv": 3700}, "link": {"frames_rx": 1}}]}
            hub.http_post("/api/rf/status-changed", rf)
            m = sub.wait_for(BASE + "/rf", 5)
            check(m is not None and jload(m[1]).get("type") == "rf_status" and jload(m[1])["slots"][0]["battery"]["percent"] == 50, "rf published and retained", str(m))
            bt = {"ok": True, "running": True, "connected": False, "hosts": []}
            with open(BT_STATUS_FILE + ".tmp", "w") as f:
                json.dump(bt, f)
            os.replace(BT_STATUS_FILE + ".tmp", BT_STATUS_FILE)
            m = sub.wait_for(BASE + "/bt", 5)
            check(m is not None and jload(m[1]).get("type") == "bt_status", "bt published", str(m))
            # button event -> remote/<slot>/button, QoS 1, not retained
            sub.drain()
            hub.http_post("/api/button-press", {"button": "VolumeUp", "action": "press", "source": "elite_rf", "slot": 1, "deviceId": "1", "command": "VolumeUp"})
            m = sub.wait_for(BASE + "/remote/1/button", 5)
            check(m is not None and jload(m[1]).get("button") == "VolumeUp" and not m[2], "button on remote/1/button", str(m))
            qos1 = [q for (t, p, rt, q) in broker.log if t == BASE + "/remote/1/button"]
            check(qos1 and qos1[-1] == 1, "button published at QoS 1", str(qos1))

            print("[-] 5.1 inventory_version follows the device file")
            version_before = retained_json(broker, BASE + "/config")["inventory_version"]
            dev_path = os.path.join(hub.data_dir, "config", "devices.json")
            with open(dev_path) as f:
                devs = json.load(f)
            devs["devices"][0]["name"] = "Living Room TV 2"
            with open(dev_path + ".tmp", "w") as f:
                json.dump(devs, f)
            os.replace(dev_path + ".tmp", dev_path)
            m = sub.wait_for(BASE + "/inventory/index", 8, lambda p: (jload(p) or {}).get("inventory_version") != version_before)
            check(m is not None, "index republished with a new inventory_version", str(m))
            m2 = sub.wait_for(BASE + "/inventory/1", 5, lambda p: (jload(p) or {}).get("name") == "Living Room TV 2")
            check(m2 is not None, "changed device chunk republished", str(m2))
            check(retained_json(broker, BASE + "/config")["inventory_version"] != version_before, "config carries the new version")

            print("[-] 5.3 command channel")
            cmd = MockClient("127.0.0.1", broker.port, "commander")
            cmd.subscribe(BASE + "/cmd/response")
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c1", "action": "get_state"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c1")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("type") == "cmd_response" and r.get("ok") is True and r.get("action") == "get_state"
                  and r.get("activity_state", {}).get("activity") == "-1", "get_state answered with the id", str(r))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c2", "action": "start_activity", "activity": "10"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c2")
            check(m is not None and jload(m[1]).get("ok") is True, "start_activity ok", str(m))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c3", "action": "start_activity", "activity": "11"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c3")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is False and r.get("code") == 409 and r.get("error") == "busy", "busy start: 409", str(r))
            sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 2)
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c4", "action": "get_snapshot"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c4")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("snapshot", {}).get("type") == "snapshot" and r["snapshot"]["hub"]["hub_id"] == UNIT_ID,
                  "get_snapshot reply carries the snapshot", str(r)[:200])
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c5", "action": "no_such"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c5")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("code") == 400, "unknown action: 400", str(r))
            cmd.publish(BASE + "/cmd", '{"id":"c6","action":"ping",', qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c6")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("code") == 400 and "malformed" in r.get("error", ""), "malformed JSON with an id: 400", str(r))
            cmd.drain()
            cmd.publish(BASE + "/cmd", json.dumps({"action": "stop_activity"}), qos=1)
            m = sub.wait_for(BASE + "/activity_state", 10, lambda p: (jload(p) or {}).get("state") == 0)
            check(m is not None, "a request without id is executed (power off)")
            check(cmd.wait_for(BASE + "/cmd/response", 1.0) is None, "and gets no reply")
            # user commands through cmd
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c7", "action": "send_command", "deviceId": "1", "command": "VolumeUp"}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c7")
            check(m is not None and jload(m[1]).get("ok") is True, "send_command ok (dry run)", str(m))
            cmd.publish(BASE + "/cmd", json.dumps({"id": "c8", "action": "rf_wake", "slot": 1}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "c8")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is False and r.get("code") == 503, "rf_wake without hub_rf: 503", str(r))

            print("[-] 5.3 ir/raw")
            st, a = hub.http_get("/api/test/ir-raw-last")
            calls_before = a.get("calls", 0)
            cmd.publish(BASE + "/ir/raw", json.dumps({"id": "r1", "frequency": 38000, "timings": [9000, -4500, 560, -560], "port": 1}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "r1")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("ok") is True and r.get("action") == "ir_raw" and r.get("count") == 4, "ir/raw answered", str(r))
            st, b = hub.http_get("/api/test/ir-raw-last")
            check(b.get("calls") == calls_before + 1 and b.get("frequency") == 38000 and b.get("first") == [9000, 4500, 560, 560], "encoder call recorded", str(b))
            cmd.publish(BASE + "/ir/raw", json.dumps({"id": "r2", "timings": [1]}), qos=1)
            m = cmd.wait_for(BASE + "/cmd/response", 5, lambda p: (jload(p) or {}).get("id") == "r2")
            r = jload(m[1]) if m else {}
            check(m is not None and r.get("code") == 400 and "timings" in r.get("error", ""), "ir/raw validation error", str(r))
            cmd.close()
            sub.close()
            st, s = hub.http_get("/api/mqtt-status")
            check(s.get("v2") is True and s.get("connected") is True, "GET /api/mqtt-status: v2 and connected", str(s))
            st, s = hub.http_get("/api/status")
            check(s.get("mqtt", {}).get("v2") is True and "mqtt_v2" in s.get("capabilities", []), "status: mqtt.v2 and the capability")
            # settings save drops the legacy fields
            st, s = hub.http_post("/api/mqtt-save", {"baseTopic": BASE, "slot1RemoteEvents": False, "haDiscovery": True})
            with open(os.path.join(hub.data_dir, "mqtt.json")) as f:
                saved = json.load(f)
            check(st == 200 and "slot1RemoteEvents" not in saved and "haDiscovery" not in saved and saved.get("baseTopic") == BASE,
                  "mqtt-save drops the legacy fields", str(saved))
    finally:
        for p in (BT_STATUS_FILE, BT_STATUS_FILE + ".tmp", mfg_path):
            try:
                os.remove(p)
            except OSError:
                pass
        broker.close()
    print("\n%s" % ("%d check(s) failed" % failed if failed else "all MQTT v2 checks passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
