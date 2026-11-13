# Activity transitions as a state machine

Design document, version 0.2 (2026-11-12). Status: proposal, not implemented. Lives in the
alexdelprete fork only until agreed with upstream.

Companion to `HA_INTEGRATION_FIRMWARE_SPEC.md`. Device names used as examples (a TV, a streamer,
a soundbar) stand for any device of that transport.

Changes in 0.2: device state reports and their rules (5.1 to 5.3), the `plugin` state source,
command classes (6), command sets that only grow (7), the command event (9), repair of a
running activity (10). Several of these ideas were first seen working in the Melodious project
(section 15).

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
| Network | A device that is up answers a TCP probe or its own API (a TV's control API, a soundbar's local API, a streamer that is reachable only when awake). | The Hub core has no network device control. Plugins bring it (next row). |
| Plugin | A plugin that talks to a device over its own protocol knows whether the device answered and often its power state, input and volume. | In progress upstream (`feat/plugin-system`): commands go to the plugin as one datagram on a UNIX socket, with a 200 ms send timeout and no reply. Nothing flows back. |
| Home Assistant | Knows the real state of many devices through their own integrations. | The Hub publishes to HA; nothing flows back. |

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
Running ──repair requested──> Repairing(step i) ──> ... ──> Running
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
- `Repairing` runs the repair steps of section 10 with the same step contract.

## 4. Step contract

Every step declares how it completes:

| `complete` | Meaning | Allowed for |
|---|---|---|
| `sent` | the command left the Hub: IR burst emitted, HID report accepted on an open link, datagram handed to a plugin | any transport |
| `acked` | the transport confirmed it: `hub_btstack` reports the HID report went out on that host's channel; an RF frame was acknowledged; a plugin answered that the device took the command | Bluetooth, RF, plugins that reply |
| `verified` | a device-state condition became true within a timeout | any transport, when the device has a state source |
| `timed` | a fixed wait, declared as such (the old delay) | any transport; the editor marks it as unverified |

`verify` (for `verified`): `{ source: <device state source>, condition: on | off | input=<name> | link_up, timeout_ms }`.

`on_fail`: `retry` (once, only for commands whose class allows it, section 6), `ask` (the Help
flow asks the user, as it does today for power toggles), `abort` (fail the transition and
report the step).

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
| `plugin` | the plugin that controls the device reports its state | needs a reply channel in the plugin protocol (5.4) |
| `ha` | Home Assistant reports the state of a chosen entity | the integration side chooses the entity; the Hub treats the report as data (5.1) |
| `assumed` | the last command sent | what stock did; the only option for IR devices with no other channel |

`bt_link` and `probe` are measured by the Hub itself. `plugin` and `ha` are reported to the Hub
by another party (a reporter) and follow the rules of 5.1 and 5.2. `assumed` is the fallback of
every other source: when a measured or reported state is `unknown`, the machine behaves as it
does today.

A verified state can lag the device by one or two seconds; this is fine for power and input,
not for keystrokes, which stay `sent` or `acked`.

### 5.1 Device state reports

A reporter (the Home Assistant integration, a plugin) sends the state of the devices it knows:

```json
{"reporter": "ha", "session": "5f1c", "seq": 17,
 "states": [
   {"device": "3", "available": true, "power": "on", "input": "HDMI 2",
    "volume": 0.35, "muted": false, "playing": "playing", "app": "Netflix"}
 ]}
```

| Field | Values | Used by |
|---|---|---|
| `device` | Hub device id | all |
| `available` | bool: the reporter can see the device | machine, UIs |
| `power` | `on`, `off`, `unknown` | machine, UIs |
| `input` | the device's input name as configured on the Hub, or null | machine, UIs |
| `volume`, `muted`, `playing`, `app` | 0..1, bool, `playing`/`paused`/`idle`, string; each may be null | UIs only |
| `session`, `seq` | reporter start id, counter per report | ordering |

Ways in, all carrying the same object: WebSocket action `device_state`, `POST /api/device-state`,
MQTT `<base>/device/state` (not retained). Way out: a `device_state` event on the bus (WebSocket
and MQTT), the same data in the snapshot, `GET /api/device-state`. A capability `device_state`
tells reporters the Hub accepts reports.

The mapping from an entity's own vocabulary to these fields is the reporter's job (for a media
player, `standby` is `off`, `idle`, `playing` and `paused` are `on`). The Hub does not know
entity ids, platforms or service names.

### 5.2 Rules for reported state

1. **Never invent state.** A field the reporter cannot read is `unknown` or null, never `off`.
   An unavailable device has `available: false` and `power: unknown`.
2. **Freshness is judged on the Hub's clock.** A reporter sends a heartbeat report at least
   every 30 s even when nothing changed. A device whose reporter has been silent for 90 s goes
   to `unknown` and is shown as "State unknown" in every UI. The reporter's own timestamps are
   not used, so clock differences cannot hide a newer report.
3. **A reporter that stops says nothing.** No final "off" on shutdown or unload: a reporter
   that crashes could not send one either, so the stale timer is the only rule.
4. **RAM only.** Reports are never written to flash and are not part of a backup. After a Hub
   restart every reported state is `unknown` until the next report.
5. **Bounded.** The reporter coalesces changes (2 s) and sends all its devices in one report.
   The Hub accepts at most one report per second per reporter, a bounded number of devices and
   a bounded body, and answers the rest with a retry hint.
6. **Ordered.** Within a `session`, a report with a lower `seq` than the last accepted one is
   dropped. A new `session` starts a new count.
7. **One reporter per device.** A second reporter for the same device is refused with an error
   that names the first.
8. **Display fields do not drive the machine.** Volume, mute, playing and app are for the UIs.
   Steps use `available`, `power` and `input` only.

### 5.3 What state changes in a sequence

Before a step, with a fresh measured or reported state:

| Step | State says | Action |
|---|---|---|
| discrete power on | `on` | skipped |
| discrete power on | `off` or `unknown` | sent |
| discrete power off | anything | sent: "everything off" must work even when the state lags the device |
| power toggle, to turn on | `on` | skipped |
| power toggle, to turn on | `off` | sent |
| power toggle, to turn off | `off` | skipped |
| power toggle, either way | `unknown` | as today (assumed state, Help asks) |
| input select | already that input, device on | skipped |
| any other command | anything | sent |

Two gains over assumed state: a device already in the wanted state is left alone, and a device
the user switched off with its own remote is switched on again where assumed state would have
skipped it. A power toggle with a known state becomes as safe as a discrete command.

A skipped step still counts as done, shortens the sequence by its wait, and still emits its
command event (section 9) with the reason, so automations that follow a button or a step run
either way.

After a step, the same state is the `verified` criterion of section 4.

### 5.4 What the plugin protocol needs

The plugin protocol being built upstream is one way. For a plugin to serve as `acked` transport
and as `plugin` state source it needs, as optional additions that old plugins can ignore:

- a request id in the command datagram and a reply datagram `{"id", "ok", "reason"}`;
- a state datagram from the plugin to the Hub, with the object of 5.1 (`reporter` = plugin id).

A plugin that sends neither stays `sent` and `assumed`, which is what every plugin is today.

## 6. Command names and classes

The machine decides by class what it may do with a command: send it again, leave it out, repeat
it while a key is held. Classes are looked up by command name, so names must be the ones the
editors, the remote and the integrations already share (the stock Logitech vocabulary that
`devices.json` and the remote button names use). A reporter or plugin that creates commands
uses these names for these meanings and puts its own vocabulary in the command's value.

| Class | Names | Retry on failure | Skipped when state matches | While held |
|---|---|---|---|---|
| discrete power | `PowerOn`, `PowerOff` | yes | `PowerOn` only (5.3) | fires once |
| power toggle | `PowerToggle`, `Power` | only with a known state | with a known state (5.3) | fires once |
| input select | `Input<Name>` (`InputHdmi1`, `InputTv`, ...) | yes | yes | fires once |
| wake | a key a device uses as power on (`Home`, `Wake`) when listed in its power-on sequence | yes | as discrete power on | fires once |
| toggle | `Mute`, `PlayPause`, `Record` | no | no | fires once |
| step | `VolumeUp`, `VolumeDown`, `ChannelUp`, `ChannelDown`, `DirectionUp`, `DirectionDown`, `DirectionLeft`, `DirectionRight`, `FastForward`, `Rewind` | no | no | repeats |
| action | `Select`, `Back`, `Menu`, `Home`, `Exit`, `Guide`, `Info`, `Play`, `Pause`, `Stop`, `SkipForward`, `SkipBack`, `Number0` to `Number9`, `Red`, `Green`, `Yellow`, `Blue`, anything not listed | no | no | fires once |

- A command can carry an explicit `class` in the device configuration; the name is only the
  default. This covers devices whose `Power` is in fact discrete, or a scene named freely.
- "Fires once" is what stops a held toggle from flipping twice. It applies to every source of a
  press: remote keys, on-screen buttons, the web remotes, the API.
- Names are matched without case, as today.

## 7. Command sets that come from outside

A device whose commands and inputs are supplied by a reporter or a plugin (not typed or learned
by the user) follows three rules:

- **Sets only grow.** A command or input the source reported once stays on the device when the
  source stops reporting it. A player that is off often stops listing its sources; removing
  them would break the buttons and activities bound to them.
- **Removal is explicit and checked.** The user removes a command, or the source removes the
  whole device. Either is refused while an activity, a button map or a sequence still refers to
  it, and the refusal names the references.
- **An unchanged set writes nothing.** A source that sends the same set again (at every start,
  typically) causes no flash write, no config version bump and no remote sync.

## 8. Bluetooth acknowledgements

`hub_btstack` gets a request id on every command and answers on the same socket:
`ACK <id> <addr>` when the report was handed to the controller on that host's open channel,
`NACK <id> <reason>` (no link, host busy, timeout). Link events (`LINK_UP <addr>`,
`LINK_DOWN <addr> <reason>`) are pushed to the daemon instead of polled from the status file.

Two rules move from the orchestrator into `hub_btstack`, where the link state is known:

- a wake key for a host whose link just opened is sent after the host has settled (about
  1.5 s), once; a key sent in the same instant as the link opens is lost by Android TV hosts;
- after a device's power-off command its host link is closed (what the stock firmware did per
  device), so the next start wakes the host with a fresh connection.

## 9. Events: commands, progress, failures

**Command event.** Every command the Hub handles for a device emits one event on the bus
(WebSocket and MQTT), whatever asked for it:

```json
{"type": "device_command", "device": "3", "command": "PowerOn",
 "origin": "activity", "activity": "1",
 "result": "skipped", "reason": "already on"}
```

`origin`: `activity` (a step), `remote` (a key or on-screen button, with the slot), `api`,
`repair`. `result`: `sent`, `acked`, `verified`, `skipped`, `failed`. The existing `button`
event stays as it is; this one is about the device command that resulted. It means "the Hub
handled this command", not "the device obeyed", unless the result is `verified`. Home Assistant
turns it into an event entity or an event on its own bus, so any Harmony key or activity step
can start an automation, also when the command was skipped.

**Progress.** `activity_progress` carries, per step: `state` (`sent`, `acked`, `verified`,
`skipped`, `waiting`, `failed`), the criterion, the elapsed time and, on failure, the reason.
The remote shows the step state on its start screen; the web UIs and Home Assistant show the
same.

**Failure.** A `Failed` transition leaves the Hub in the last verified state and offers the
user the repair of the step, a retry of the transition, or a stop.

## 10. Repair of a running activity

Today Help re-sends the running activity's commands and asks one question per toggled device.
With the step contract and device state it becomes a set of small, named repairs:

| Repair | What it does |
|---|---|
| one device, power | sends that device's power-on step only ("It's off") |
| one device, input | sends that device's input step for the running activity only ("Wrong input") |
| everything | walks the activity's power and input steps and sends only those whose state does not match; without a state source for a device, sends its step |

- Each repair is a short sequence in the `Repairing` state, with the same criteria as a start.
  Nothing else changes: no other device is touched, the activity stays running.
- The list of repairs for the running activity is available to every client (`GET
  /api/activity/help` or the `preview` of today's dry run), one entry per device and kind, each
  with a hint from the device state: "already on", "on HDMI 1", "state unknown". A repair the
  state says is not needed is offered dimmed, not hidden: the state can be wrong.
- The question Help asks for a power toggle is asked only when the device's state is
  `unknown`. With a known state the machine decides (5.3).
- The remote's Help screen, the web UIs and the integration use the same list.

## 11. Changes by component

| Component | Change | Size |
|---|---|---|
| `orchestrator.c`, `orchestrator_queue.c` | the machine of section 3; the public API (`orch_switch_activity`, `orch_power_off`, `orch_repair_activity`, dry-run previews) kept | large |
| `hub_btstack` IPC | request ids, ACK/NACK replies, pushed link events; the two rules of section 8 | medium |
| `hw_action.c` | per-transport delivery result instead of 0/-1; command classes; the command event | medium |
| device state module (new) | sources `bt_link`, `probe`, reported states with the rules of 5.2, the `device_state` event and routes | medium |
| plugin manager | optional reply and state datagrams (5.4) | small |
| config schema | step fields `complete`, `verify`, `on_fail`; device fields `stateSource`, per-command `class`; loader maps old steps | medium, with migration |
| config store | grow-only command sets for sourced devices, reference check on removal, no-op on an unchanged set (7) | small |
| Help | the repair list and single repairs of section 10 | medium |
| web UIs (legacy and new) | step editor and device editor fields; the preview shows criteria; state strip and repair sheet (`UI_DESIGN.md`) | medium |
| MQTT and the HA integration | the integration reports chosen entities (5.1) and exposes `device_command` | small, medium |
| remote | step state on the start screen; repair list on the Help screen | small |
| docs, OpenAPI, fixtures, tests | mock devices that acknowledge and expose state, so the co-simulation drives the machine end to end | medium |

## 12. Limits

- An IR device without a state source cannot be verified; its waits stay `timed`. A device
  warm-up is physics, but with a source the wait ends as soon as the device is up and fails if
  it never comes, which is the gain over a fixed delay.
- A reported source makes those steps depend on the reporter being up. The rule is fixed, not
  configurable per step: stale or missing state is `unknown`, and `unknown` behaves as today.
  An activity always starts.
- Reported state can be forged by anything allowed to call the API. It is accepted only from an
  authenticated client with the operator role or higher, like the commands themselves.
- This touches the core of the firmware and the configuration format. It needs upstream
  agreement on the design before code, and would land as a series of PRs behind a capability
  flag (`fsm`), with the old behaviour kept until the user opts in per activity.

## 13. Phasing

1. The machine with Bluetooth acknowledgements: orchestrator rewrite, `hub_btstack` reply
   protocol, one step at a time, command classes, old configurations unchanged. This alone
   fixes the known problems of today's transitions: a host link left open after power off, the
   lost wake key, a power off refused during a start.
2. Device state: sources `bt_link` and `probe`; reported state (`ha`, `plugin`) with its
   rules; `verified` steps, `WaitState` and the skip table; the `device_state` and
   `device_command` events.
3. Repair list and single repairs; state strip and repair sheet in the UIs; step state on the
   remote.
4. Editors, documentation, and a migration that turns delays into verified waits where a
   source exists. Grow-only command sets when the first source that creates devices exists.

## 14. Open questions

- Which hosts beyond Android TV lose a key sent at link open, and whether 1.5 s is enough for
  all of them.
- Whether `probe` should also drive wake (Wake-on-LAN for devices that support it), or whether
  that belongs in a plugin.
- How Help's toggle questions integrate with `on_fail: ask` on the remote's screen.
- Whether upstream wants the reply and state datagrams in the first version of the plugin
  protocol; adding them later is compatible, but plugin authors would have to revisit.
- Whether Home Assistant entities should also become Hub devices (commands sent to HA), as
  Melodious does. This document only takes state from HA; commands to HA would be a plugin or
  an integration feature of its own.

## 15. Prior art

Melodious (`schlambos/harmony-hub-control`, with the `ha-melodious` integration) keeps the
stock Logitech engine and adds Home Assistant devices to it. It showed in practice: a RAM-only
state channel from Home Assistant with a staleness rule and "unknown, never off"; leaving out a
power-on or input command when the state already matches (decided on the Home Assistant side,
for devices Home Assistant controls); an event per command for automations; command sets that
only grow; and a per-device "Fix it". Sections 5.1 to 5.3, 7, 9 and 10 take those ideas and
apply them inside the Hub's own engine, to any device with a state source. No code or text is
taken from it.
