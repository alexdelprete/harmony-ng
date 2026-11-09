# Activity transitions as a state machine

Design document, version 0.1 (2026-11-09). Status: proposal, not implemented. Lives in the
alexdelprete fork only until agreed with upstream.

Companion to `HA_INTEGRATION_FIRMWARE_SPEC.md`. Device names used as examples (a TV, a streamer,
a soundbar) stand for any device of that transport.

## 1. Goal

Today an activity start or stop is a queue of steps fired on timers: send, wait the step's delay,
send the next, with no knowledge of whether a command arrived or had its effect. Flaky results are
patched with repeated keys and longer delays. The goal is a transition that:

- runs as a finite state machine, one step at a time;
- gives every step an explicit completion criterion (sent, acknowledged, or verified against the
  device's state);
- treats a missed criterion as a state with a decision (retry, ask, fail), not as silence;
- keeps timed waits only where no feedback channel exists, and says so in the configuration.

## 2. What each channel can report

| Channel | Feedback the Hub can get | Today in harmony-ng |
|---|---|---|
| Bluetooth HID (hosts such as an Android TV box or a TV) | Link open and close events with a reason code. Delivery over L2CAP is reliable: a report accepted on an open link reaches the host. Whether the host is ready to act on it is not reported. | `hub_daemon` writes commands to a pipe and a datagram socket; `hub_btstack` answers nothing and only counts `sent`. |
| Infrared | Nothing from the device. The Hub's receiver sees its own blaster, so emission can be verified, reception cannot. | `ir_i2s_blast_blob()` returns when the burst is out. |
| RF remote | Every frame is acknowledged; config version, firmware version and battery reports. | Synchronous at the protocol level already. |
| Network | A device that is up answers a TCP probe or its own API (a TV's control API, a soundbar's local API, a streamer that is reachable only when awake). | The Hub has no network device control. |
| Home Assistant | Knows the real state of many devices through their own integrations. | The Hub publishes to HA; nothing flows back. The MQTT command channel can carry device states into the Hub. |

The stock firmware was not synchronous either: its activity engine ran the command list in
order with fixed sleeps, checked a result code per command (a Bluetooth device not reachable was
retried), and the power state in its state digest was assumed from the commands sent, not
measured. This design goes beyond stock.

## 3. The machine

```
Idle ──start──> Starting(step i) ──> WaitAck(i) ──> WaitState(i) ──> Starting(i+1) ... ──> Running
Running ──stop──> Stopping(step i) ──> WaitAck(i) ──> WaitState(i) ──> ... ──> Idle
any Wait* ──timeout──> Repair(i) ──retry──> Starting/Stopping(i)
                                ──ask────> (Help question, answer resumes or fails)
                                ──fail───> Failed(i) ──user──> Idle or Running (last verified)
Starting ──stop requested──> Stopping (devices already powered on are powered off)
```

- One step is active at a time. The background queue and the anchored tail steps of the
  current orchestrator are removed; a step that must run after a point in time becomes a step
  with a `verified` or `timed` criterion.
- `Starting` and `Stopping` execute the step and move to `WaitAck` when the transport can
  acknowledge, else to `WaitState` when the step has a verification, else to the next step.
- A stop requested while starting is legal: the machine moves to `Stopping` with the steps
  for the devices the start had already powered on.
- The current activity file holds the activity being started from the first step; a failed or
  cancelled start restores the previous one.

## 4. Step contract

Every step declares how it completes:

| `complete` | Meaning | Allowed for |
|---|---|---|
| `sent` | the command left the Hub: IR burst emitted, HID report accepted on an open link | any transport |
| `acked` | the transport confirmed it: `hub_btstack` reports the HID report went out on that host's channel; an RF frame was acknowledged | Bluetooth, RF |
| `verified` | a device-state condition became true within a timeout | any transport, when the device has a state source |
| `timed` | a fixed wait, declared as such (the old delay) | any transport; the editor marks it as unverified |

`verify` (for `verified`): `{ source: <device state source>, condition: on | off | input=<name> | link_up, timeout_ms }`.

`on_fail`: `retry` (once, only for idempotent commands: discrete power, input select, a wake
key), `ask` (the Help flow asks the user, as it does today for power toggles), `abort` (fail
the transition and report the step).

Example, a start sequence for an activity with a TV (IR, probe over the network), a Bluetooth
streamer and an always-on soundbar:

```json
[
  {"type": "Command", "deviceId": "tv", "command": "PowerOn",
   "complete": "verified", "verify": {"source": "device", "condition": "on", "timeout_ms": 15000}, "on_fail": "retry"},
  {"type": "Command", "deviceId": "streamer", "command": "Home",
   "complete": "acked", "on_fail": "retry"},
  {"type": "Command", "deviceId": "tv", "command": "InputHdmi2",
   "complete": "verified", "verify": {"source": "device", "condition": "input=HDMI2", "timeout_ms": 5000}, "on_fail": "ask"}
]
```

Old configurations load unchanged: a step without `complete` is `sent` followed by a `timed`
wait of its old delay. Nothing changes for them until the user edits a step.

## 5. Device state model

Each device has a `powerState` (`on`, `off`, `unknown`) and, where it applies, an `input`, with
one source:

| `stateSource` | How the state is known | Notes |
|---|---|---|
| `bt_link` | the device's Bluetooth link to the Hub is up or down | a TV paired as a Bluetooth host drops its link when it powers off and accepts a connection when on: a real "TV on" signal with no network involved |
| `probe` | `{host, port}` TCP connect, or `{url}` expecting a 2xx | a streamer reachable only when awake, a TV's control port, a soundbar's local API |
| `ha` | Home Assistant publishes the state of a chosen entity to `<base>/device/<id>/state` | the integration side chooses the entity; the Hub treats the payload as data |
| `assumed` | the last command sent | what stock did; the only option for IR devices with no other channel |

The orchestrator consults the state before a step (a `PowerOn` to a device already `on` is
skipped for discrete commands, kept for toggles unless the state is known) and after it (the
`verified` criterion). A verified state can lag the device by one or two seconds; this is fine
for power and input, not for keystrokes, which stay `sent` or `acked`.

## 6. Bluetooth acknowledgements

`hub_btstack` gets a request id on every command and answers on the same socket:
`ACK <id> <addr>` when the report was handed to the controller on that host's open channel,
`NACK <id> <reason>` (no link, host busy, timeout). Link events (`LINK_UP <addr>`,
`LINK_DOWN <addr> <reason>`) are pushed to the daemon instead of polled from the status file.

Two rules move from the orchestrator into `hub_btstack`, where the link state is known:

- a wake key for a host whose link just opened is sent after the host has settled (about
  1.5 s), once; a key sent in the same instant as the link opens is lost by Android TV hosts;
- after a device's power-off command its host link is closed (what the stock firmware did per
  device), so the next start wakes the host with a fresh connection.

## 7. Progress and failure reporting

`activity_progress` carries, per step: `state` (`sent`, `acked`, `verified`, `waiting`,
`failed`), the criterion, the elapsed time and, on failure, the reason. The remote shows the
step state on its start screen; the web UIs and Home Assistant show the same.

A `Failed` transition leaves the Hub in the last verified state and offers the user the repair
of the step (Help), a retry of the transition, or a stop.

## 8. Changes by component

| Component | Change | Size |
|---|---|---|
| `orchestrator.c`, `orchestrator_queue.c` | the machine of section 3; the public API (`orch_switch_activity`, `orch_power_off`, `orch_repair_activity`, dry-run previews) kept | large |
| `hub_btstack` IPC | request ids, ACK/NACK replies, pushed link events; the two rules of section 6 | medium |
| `hw_action.c` | per-transport delivery result instead of 0/-1; the device state module with the probes | medium |
| config schema | step fields `complete`, `verify`, `on_fail`; device field `stateSource`; loader maps old steps | medium, with migration |
| web UIs (legacy and new) | step editor and device editor fields; the preview shows criteria | medium |
| MQTT and the HA integration | state topic the integration writes; the integration publishes the chosen entities | small, small |
| remote | step state on the start screen | small |
| docs, OpenAPI, fixtures, tests | mock devices that acknowledge and expose state, so the co-simulation drives the machine end to end | medium |

## 9. Limits

- An IR device without a state source cannot be verified; its waits stay `timed`. A device
  warm-up is physics, but with a source the wait ends as soon as the device is up and fails if
  it never comes, which is the gain over a fixed delay.
- `ha` as a source makes the Hub depend on Home Assistant being up for those steps; the
  configuration must say what happens without it (fall back to `timed`).
- This touches the core of the firmware and the configuration format. It needs upstream
  agreement on the design before code, and would land as a series of PRs behind a capability
  flag (`fsm`), with the old behaviour kept until the user opts in per activity.

## 10. Phasing

1. The machine with Bluetooth acknowledgements: orchestrator rewrite, `hub_btstack` reply
   protocol, one step at a time, old configurations unchanged. This alone fixes the known
   problems of today's transitions: a host link left open after power off, the lost wake key, a
   power off refused during a start.
2. Device state sources: `bt_link`, `probe`, `ha`; `verified` steps and `WaitState`.
3. Editors, documentation, remote display, and a migration that turns delays into probes
   where a source exists.

## 11. Open questions

- Which hosts beyond Android TV lose a key sent at link open, and whether 1.5 s is enough for
  all of them.
- Whether `probe` should also drive wake (Wake-on-LAN for devices that support it).
- How Help's toggle questions integrate with `on_fail: ask` on the remote's screen.
