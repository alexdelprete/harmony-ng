#!/usr/bin/env python3
"""
WebSocket event tests against hub_daemon_test (spec docs/HA_INTEGRATION_FIRMWARE_SPEC.md,
milestone 1): activity_state and activity_progress on an activity switch (3.2).

Usage: python3 tests/test_ws_events.py            (builds hub_daemon_test if missing)
Pure stdlib; the daemon runs on a temporary mock root with tests/fixtures/ha/sample_config.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hub_harness import HubHarness  # noqa: E402

PORT = 8997
g_passed = 0
g_failed = 0


def check(cond, name, detail=""):
    global g_passed, g_failed
    if cond:
        g_passed += 1
        print("  PASS %s" % name)
    else:
        g_failed += 1
        print("  FAIL %s %s" % (name, detail))


def states_of(frames):
    return [(m.get("state"), m.get("activity")) for m in frames if m.get("type") == "activity_state"]


def test_activity_switch(hub):
    print("[-] 3.2 activity_state and activity_progress on a dry-run activity switch")
    ws = hub.ws()
    time.sleep(0.2)
    ws.drain(0.3)
    # The daemon runs with --dry-run (IR simulated); "dry_run" in the body would only preview
    st, resp = hub.http_post("/api/activity", {"activity": "10"})
    check(st == 200 and resp.get("success") is True, "POST /api/activity 10", "%s %s" % (st, resp))
    running, frames = ws.wait_for("activity_state", 10, lambda m: m.get("state") == 2 and m.get("activity") == "10")
    check(running is not None, "activity_state running for 10 arrives", str(frames))
    sts = states_of(frames)
    check(sts and sts[0] == (1, "-1"), "first activity_state is starting with the old activity -1", str(sts))
    check(sts and sts[-1] == (2, "10"), "last activity_state is running with activity 10", str(sts))
    progress = [m for m in frames if m.get("type") == "activity_progress"]
    check(len(progress) >= 2, "activity_progress frames between them (%d)" % len(progress), str(frames))
    if progress:
        p = progress[0]
        check(p.get("activity") == "10" and p.get("activityId") == "10" and "stepDesc" in p and "totalSteps" in p,
              "activity_progress keeps its fields", str(p))
    if running:
        check(running.get("stateName") == "running" and running.get("busy") is False,
              "running frame: stateName and busy", str(running))
        check(isinstance(running.get("seq"), int) and isinstance(running.get("ts"), int),
              "seq and ts are integers", str(running))
    seqs = [m["seq"] for m in frames if "seq" in m]
    check(seqs == sorted(seqs) and len(set(seqs)) == len(seqs), "seq strictly increasing", str(seqs))
    st, cur = hub.http_get("/api/activity")
    check(st == 200 and cur.get("activity") == "10" and cur.get("state") == 2, "GET /api/activity agrees", str(cur))

    # Power off: stopping, then idle with activity -1
    st, resp = hub.http_post("/api/poweroff", {})
    check(st == 200, "POST /api/poweroff", "%s %s" % (st, resp))
    idle, frames = ws.wait_for("activity_state", 10, lambda m: m.get("state") == 0)
    sts = states_of(frames)
    check(sts and sts[0] == (3, "10"), "stopping with activity 10 first", str(sts))
    check(idle is not None and idle.get("activity") == "-1" and idle.get("stateName") == "idle",
          "idle with activity -1 last", str(idle))

    # A second client gets no replay: the bus pushes changes only
    ws2 = hub.ws()
    replay = ws2.drain(0.5)
    check(all(m.get("type") != "activity_state" for m in replay),
          "no activity_state replay for a new client without a switch", str(replay))
    ws2.close()
    ws.close()


def main():
    with HubHarness(port=PORT) as hub:
        test_activity_switch(hub)
    print("\n%d passed, %d failed" % (g_passed, g_failed))
    return 1 if g_failed else 0


if __name__ == "__main__":
    sys.exit(main())
