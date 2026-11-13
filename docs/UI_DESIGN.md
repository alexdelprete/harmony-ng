# HarmonyNG app design

Design document, version 0.1 (2026-11-12). Status: proposal, not implemented. Lives in the
alexdelprete fork only until agreed with upstream.

One design for every HarmonyNG client: the web app the Hub serves, the Android app, and a
desktop app (macOS first). It covers identity, tokens, layout, every screen, components,
behaviour and platform notes, so that a screen can be built from this document without asking
what it should look like.

The direction (a phone remote first, four tabs, Material 3, sheets that do one job) is inspired
by the Melodious apps for the stock Harmony firmware. Only the ideas are taken: palette, values,
wording, icons and code here are HarmonyNG's own.

## 1. Principles

1. **A remote first.** The app opens on what is running and how to control it. Setup is one
   tap away, never in the way.
2. **One obvious thing per screen.** One primary action, in one place, in the primary colour.
3. **Always say what is on and what a key will do.** Running activity, device state and the
   command behind every key are visible, not guessed.
4. **Never show a guess as a fact.** A state the Hub does not know is "unknown", never "off".
5. **Same app everywhere.** Phone, tablet, desktop and the wrapped apps differ in layout, not
   in structure, names or behaviour.
6. **The Hub is small.** No CDN, no web fonts, no framework runtime, nothing fetched from the
   internet to draw a screen.
7. **Live.** Screens follow the Hub's event stream. Nothing needs a reload, and nothing polls
   where an event exists.

## 2. Identity

- **Name:** HarmonyNG. "Harmony Hub", "Harmony Elite" and the other model names stay the names
  of the hardware.
- **Mark:** the existing HarmonyNG icon. One 64 px favicon and one 180 px home-screen icon,
  inlined in the page.
- **Feel:** calm, dark by default, large touch targets, little text. The remote looks like a
  keypad, not like a photo of a remote.
- **Voice:** short sentences, sentence case, no jargon where a plain word exists ("Turn off",
  not "Power off activity"). The same thing has the same name on every screen and in every
  language file.

## 3. Colour

Two themes, dark (default) and light, chosen by the system setting with a manual override in
Settings. The values keep today's HarmonyNG look (deep navy, sky blue accent) and give every
colour a role.

### 3.1 Tokens

| Token | Dark | Light | Role |
|---|---|---|---|
| `--canvas` | `#0a0e17` | `#f6f8fb` | page background |
| `--nav` | `#0f1522` | `#eef2f7` | bottom bar, rail, top bar |
| `--card` | `#161f2e` | `#ffffff` | cards, list groups, sheets |
| `--key` | `#1c2739` | `#e9eef5` | remote keys, rockers, tiles |
| `--key-raised` | `#26344d` | `#dbe3ee` | pressed key, OK, secondary buttons |
| `--divider` | `#243045` | `#dde4ec` | hairlines between rows |
| `--outline` | `#64748b` | `#64748b` | field borders, focus ring outer layer |
| `--text` | `#f1f5f9` | `#0f172a` | titles, body |
| `--text-variant` | `#9ca3af` | `#475569` | subtitles, captions, inactive icons |
| `--primary` | `#38bdf8` | `#0369a1` | main action, selected tab, links, section labels |
| `--on-primary` | `#04263a` | `#ffffff` | text on a primary fill |
| `--primary-container` | `#0c4a6e` | `#d6efff` | selected segment, tonal buttons, tab pill |
| `--on-primary-container` | `#cfeafc` | `#0c4a6e` | text on it |
| `--running` | `#10283d` | `#e0f2fe` | the running activity's card, info notes |
| `--live` | `#34d399` | `#047857` | running, connected, in sync |
| `--warning` | `#fbbf24` | `#b45309` | needs attention, state unknown, dry run |
| `--error` | `#fda4af` | `#be123c` | failures, error text |
| `--power-container` | `#4c1420` | `#ffe4e6` | power and destructive buttons |
| `--on-power-container` | `#fda4af` | `#9f1239` | text and icon on them |
| `--inverse` / `--on-inverse` | `#e2e8f0` / `#0f172a` | `#1e293b` / `#f1f5f9` | snackbars |
| `--key-red` `--key-green` `--key-yellow` `--key-blue` | `#ef4444` `#22c55e` `#eab308` `#3b82f6` | same | colour keys |

Contrast, computed (WCAG): body text on every surface is above 11:1 in both themes; variant
text is 6.5:1 or more on canvas and cards and 4.9:1 at its lowest (on a raised key, dark);
primary as text is 7.0:1 or more in dark and 5.1:1 or more in light, on canvas, card, key and
the running card; text on primary, on primary container and on the power container is above
5.9:1; `--live`, `--warning` and `--error` as text on a card are above 5:1. `--outline` on
canvas is 4.1:1 (dark) and 4.5:1 (light), enough for borders, not used for text.

### 3.2 Rules

- **Primary** is for the one main action of a screen, the selected tab and section labels.
- **Green means running, connected or in sync.** Nothing else is green.
- **Power and destructive actions use the power container**, never primary.
- **Amber means "look at this"**: unknown state, a remote out of sync, dry-run mode.
- A key with no command stays in place, at 35% opacity, and does nothing.
- No colour outside the tokens. A new need gets a new token with both theme values.
- State is never shown by colour alone: a dot or tint always comes with a word or an icon.

### 3.3 From today's tokens

`--bg-main` becomes `--canvas`, `--bg-card` `--card`, `--bg-btn` `--key-raised`, `--accent`
`--primary` (dark), `--primary` (`#0284c7`) is replaced by `#0369a1` as the light primary for
contrast, `--success` `--live`, `--danger` the power container pair, `--text-dim`
`--text-variant`. The glow tokens are dropped. During the move the old names stay as aliases.

## 4. Type

System font stack (Roboto on Android, San Francisco on Apple, Segoe on Windows), no shipped
fonts. A monospace stack for codes, logs and addresses.

| Use | Size / weight |
|---|---|
| Screen title | 28 / 500 |
| Remote title (running activity) | 24 / 500 |
| Card title, empty-state title | 22 / 500 |
| Sheet and dialog title | 20 / 500 |
| Row title | 16 |
| Body, buttons | 15 (buttons 500) |
| Section label | 14 / 500, primary |
| Subtitle, hint | 13 |
| Key caption, badge | 12 |
| Overline ("VOL", "CH", "RUNNING NOW") | 11 / 600, letter spacing 1 px, upper case |

Sentence case everywhere. Nothing below 12 px; body text never below 13 px. Text must survive
the user's font scale up to 200%: rows grow, nothing is clipped. Strings in German, French and
Dutch run 30% longer than English: no fixed-width labels.

## 5. Shape, spacing, depth

- **Radius:** 8 (chips, fields), 14 (keys, small cards), 20 (cards, list groups), 28 (sheets,
  dialogs), full (pills, round buttons, D-pad, OK, power).
- **Spacing:** 4 px grid, steps 4 8 12 16 24 32 48. Page gutter 16 px on phones, 24 px from
  600 px.
- **Touch targets:** 48 px minimum; remote keys 56 px or more; list rows 64 px or more.
- **Depth:** surfaces are told apart by tone, not shadow. One shadow level only, for sheets,
  menus and snackbars. No glows, no gradients.

## 6. Layout and navigation

### 6.1 Window sizes

| Class | Width | Navigation | Content |
|---|---|---|---|
| Compact | under 600 px | bottom bar, 80 px, four items with a pill on the selected one | one column |
| Medium | 600 to 839 px | bottom bar | one column, max 720 px, centred |
| Expanded | 840 px and up | rail on the left, 88 px, mark on top | list and detail side by side where it helps; from 1100 px the remote shows "More keys" in a panel beside it |

Lists and forms are never wider than 720 px. The remote keypad is never wider than 440 px.
Safe-area insets are respected on every edge. Landscape phones use the compact layout with the
remote scaled to the height.

### 6.2 Tabs

| Tab | Route | Holds |
|---|---|---|
| Remote | `#/remote` | the activity picker when nothing runs, the running activity's remote otherwise |
| Activities | `#/activities` | list, start and stop, the activity editor, button maps |
| Devices | `#/devices` | list, a device's own remote and commands, adding and editing devices, IR tools |
| Settings | `#/settings` | the Hub, remotes, Bluetooth, integrations, network, plugins, users, backup, system |

- Every screen has its own route (`#/devices/<id>`, `#/devices/<id>/commands`,
  `#/activities/<id>/edit`, `#/settings/remotes/<slot>`), so it can be bookmarked, linked from
  Home Assistant and restored after a reload. Old routes redirect.
- A sub-page has a top bar (back arrow, title, at most two actions and a menu) and hides the
  bottom bar on compact screens. Back returns to where the user came from.
- The Hub name, connection state and language are not in a permanent header. Connection shows
  as a dot next to the screen title; Hub and language live in Settings.

### 6.3 Where today's sections go

| Today (Setup) | New place |
|---|---|
| Activities Setup | Activities tab |
| Devices Studio, Harmony DB Search, IR Lab | Devices tab: add device, device page, tools |
| Harmony (RF) Remote, BT Remote | Settings, Remotes |
| Bluetooth Hosts, BT keyboard | Settings, Bluetooth; the keyboard is a mode of the Remote tab |
| MQTT & Home Assistant | Settings, Integrations |
| Network | Settings, Network |
| Plugins & Marketplace | Settings, Plugins; plugin device types appear in Devices, add device |
| Users & Auth | Settings, Users and access |
| Backup & Restore | Settings, Backup |
| System Admin | Settings, System |
| Hub Management | Settings, Hubs (and the first-run connect screen in the wrapped apps) |

## 7. Screens

### 7.1 Remote, nothing running

Title "Everything's off", subtitle "What do you want to do?". One card per activity (icon tile,
name, its devices in one line, a start glyph). Tapping a card starts it. With no activities, an
empty state with one button, "Create an activity".

### 7.2 Remote, starting or stopping

The activity name, "Starting..." or "Turning off...", and the step list as it runs: device,
command, state (sent, confirmed, skipped, waiting, failed). A request made meanwhile is shown
as "Next: <activity>". A failed step offers Retry, Fix, Stop.

### 7.3 Remote, running

Top to bottom:

1. **Header:** activity name with a chevron (opens Switch activity), the devices in one line
   with a live dot, and three round buttons: on-screen buttons editor, Fix it, Power.
2. **State strip** (when any device has a known or reported state): chips such as "TV on",
   "HDMI 2", "Vol 35", "Playing". See section 9.
3. **Mode tabs:** Buttons, Keyboard (when the activity has a Bluetooth or text-capable device),
   Touchpad (apps with touch input).
4. **Keypad:** transport row (rewind, play, pause, forward, more), D-pad ring with OK, a VOL
   rocker on the left, a CH rocker on the right, and Back, Menu or Home, Exit, Mute between
   them. The D-pad takes the spare height.
5. **Status line:** what the last key did, "OK → Shield · Select"; on desktop it previews the
   key under the pointer.

**More keys** (sheet; a side panel from 1100 px): the activity's on-screen buttons, Guide,
Info, DVR, Exit, record and stop, colour keys, number pad, then "All device commands" and
"Send log".

**Switch activity** (sheet): the other activities, and "Turn everything off" on the power
container.

**Fix it** (sheet): "Something not right?", one card per device with the fixes that apply
("It's off", "Switch to HDMI 2"), each with the state hint ("already on", "state unknown"), and
"Resend everything that's needed" on top. A fix the state says is not needed is dimmed, still
usable.

**Power** turns the activity off at once, without a confirmation, as the remote's Off key does.

### 7.4 Activities

- Title with a connection dot, a round "+" button.
- The running activity as a card on `--running`: overline "Running now", icon, name, devices,
  buttons "Open remote" (primary) and "Turn off" (power container), small actions for buttons
  and edit.
- "Start something else": one row per activity with icon tile, name, devices or a warning ("No
  buttons set up"), a round start button and a menu: Edit, Buttons, Try the buttons, Sequence,
  Move up, Move down, Duplicate, Delete.
- **Editor** (five steps, section 10.1): Name and icon, Devices and roles, Inputs, Buttons,
  Review. An "Advanced" entry opens the full sequence editor: steps with device, command,
  completion rule (sent, confirmed, verified, timed) and what to do on failure, plus the dry
  run preview.
- **Buttons editor:** three tabs, On-screen (ordered list, add, reorder), Hard keys (list by
  group, each row "key → device · command", with a hold action where supported), Gestures
  (where the remote supports them). Per-remote differences are shown per row ("Elite differs").

### 7.5 Devices

- Title, round "+", a search field over devices and commands.
- One list group; each row: icon tile, name, "model · N commands", and a badge for how it is
  controlled: IR, Bluetooth, Network, Plugin, Home Assistant. When the device has a known
  state the subtitle starts with it ("On · HDMI 1").
- **Device page:** top bar with name, settings and power; state strip; tabs "Remote" (the same
  keypad, sending straight to the device, outside any activity) and "Commands (N)" (searchable
  list, tap to send, menu per command: rename, edit code, class, delete). Actions: Learn a
  command, Import codes, Send all.
- **Add device** (sheet with choices): Infrared (search the database, learn, paste or import
  a file), Bluetooth (pair the Hub as keyboard and remote), a plugin type (fields from the
  plugin's manifest), Home Assistant (when the integration offers devices).
- **Device settings:** power (discrete, toggle, none; always on), warm-up times, inputs, state
  source, key repeat. A list page with a Save bar (section 10.2).
- **Tools:** IR lab (send and compare codes), Send all (walk a command list with a gap, stop
  any time), capture monitor.

### 7.6 Settings

A list of groups; each row has an icon tile, a title, one line of status and a chevron.

| Row | Status line | Page |
|---|---|---|
| Hub | name, version, "connected" | name, time zone, update, dry-run switch, restart |
| Remotes | "Harmony Elite · 87% · in sync" | one card per slot: model, battery and charging, voltage, firmware, last seen, sync state; actions Wake, Sleep, Sync now, Update, Unpair; pairing window; remote settings (brightness, timeout, night window, theme, language, standby); button maps |
| Bluetooth | "2 hosts connected" | hosts with state, connect, disconnect, pair, forget; default host |
| Integrations | "MQTT connected · Home Assistant" | MQTT broker and discovery; Logitech-compatible mode; API tokens for integrations |
| Network | "Wi-Fi · 192.168.1.20" | Wi-Fi, Ethernet, discovery (mDNS) |
| Plugins | "3 installed" | Installed, Marketplace, Repositories |
| Users and access | "Sign-in required" | users, roles, tokens, sign-in on or off |
| Backup | "Last backup 3 days ago" | download, restore, rolling backups |
| System | "Uptime 4 d · 2 MB free" | status, storage, services, logs, reboot |
| Hubs | "Harmony Living" | known Hubs, discover, switch (wrapped apps and multi-Hub users) |
| App | theme, language | theme (system, dark, light), language, haptics, about |

### 7.7 First run and sign-in

- **Web, served by the Hub:** no connect screen. If sign-in is required, a sign-in card on the
  canvas (user, password, "Sign in"); a wrong password is an inline error, not a dialog.
- **Wrapped apps:** a connect screen: Hubs found on the network as cards, "Enter address"
  below. The chosen Hub is remembered.
- **Empty Hub:** the Remote tab offers "Add your first device", then "Create an activity".

## 8. Components

| Component | Description |
|---|---|
| Screen title | 28 px, optional subtitle with a status dot, optional round action on the right |
| Section label | 14 px, primary, above a list group |
| List group and row | card with 20 px radius; rows 64 px or more with hairlines; leading icon tile, title, subtitle, trailing value, chevron, switch or menu |
| Icon tile | 40 px rounded square on `--key`, one outline icon; tinted `--primary-container` for the running item |
| Status dot | 8 px: live (green), attention (amber), off or unknown (variant); always next to a word |
| Badge | 12 px label in a small pill: IR, Bluetooth, Network, Plugin, Home Assistant; "Admin" on admin-only rows |
| Chip | 32 px pill; state chips (read only) and filter chips (pressed state) |
| Buttons | filled (primary), tonal (primary container), outlined, text, all pills 40 to 52 px high; round icon button 48 px; power button on the power container |
| Segmented tabs | pill track on `--key`, selected segment on `--primary-container` |
| Field | label above, 8 px radius, outline border, hint or error below; password fields have a show toggle |
| Switch row | a row whose trailing control is a switch; the whole row toggles |
| Radio rows | one choice among rows in a group |
| Stepper | minus and plus around a number that can also be typed (delays, repeats) |
| Sheet | rises from the bottom with a handle on compact; a centred card (max 560 px) from 840 px; closes on scrim tap, Escape, or when its one job is done |
| Dialog | only to confirm something destructive or to ask a question the flow cannot continue without; the safe choice has focus |
| Menu | dropped from its "more" button; arrow keys move, Escape closes |
| Snackbar | inverse surface at the bottom; 4 s for information, 8 s with an action for errors |
| Banner | full-width strip under the top bar for a standing condition: dry run on, Hub unreachable, update ready |
| Progress | thin indeterminate bar under the top bar for saves; a stepped meter for runs with known length (update, Send all, RF update) |
| Empty state | icon, one sentence, one button |
| Remote keys | key, icon key, captioned key, D-pad ring and OK, rocker, colour key, number key |
| Step list | rows with device, command and a state glyph, used by start, stop, repair and the dry run |
| Command picker | sheet with device chips on top, a search field, the command list with a check on the current one; closes on pick |

## 9. Showing state

- **Activity:** off, starting, running, stopping, failed, plus the queued request. Driven by
  the `activity_state` and `activity_progress` events.
- **Device:** on, off, unknown; input; and, where reported, volume, mute, playing, app. Shown
  as the state strip on the remote and device page, and as the first words of the subtitle in
  lists. A device with no state source shows no state at all; a device whose source went silent
  shows "State unknown" in amber. "Off" appears only when a source says off.
- **Hub connection:** a dot by the title: connected (green), connecting (amber, after 2 s),
  unreachable (a banner with Retry; controls stay visible but disabled).
- **Remote control (hardware):** battery percent and charging, in sync or "update pending",
  last seen; asleep is normal, not an error.
- **Bluetooth host:** connected, connecting, not connected, pairing.
- **MQTT, plugins, update:** one status word each in their Settings row.
- Everything above updates from the event stream while the screen is open.

## 10. Behaviour

### 10.1 Keys

- A key fires when the finger lifts; sliding off cancels.
- A key with a hold action fires that action after 500 ms instead and shows a small dot.
- Volume, channel and D-pad fire on touch and repeat while held (start after 400 ms), ending on
  lift or slide-off. The app sends press, hold and release so the Hub repeats natively.
- Toggles (power, mute, play/pause) fire once however long they are held.
- Feedback: the key brightens, the device vibrates briefly where it can, the status line shows
  what was sent. A failed send is a snackbar.
- Keyboard users: every key is a button with a name; arrow keys, Enter, Backspace and Escape
  map to D-pad, OK and Back when the keypad has focus; media keys map to transport.

### 10.2 Flows

**Wizard** (new activity, new device, pairing):
- top bar with close, the flow's title and "Step n of m"; a segmented progress bar under it,
  one segment per step; a filled step can be reopened from its segment;
- each step opens with one question (26 px) and one line of help;
- choices are cards (one tap), chips (short options) or a sheet (long lists);
- a bottom bar with Back and a Next that says where it goes ("Next: inputs") and, on the last
  step, what it does ("Save activity");
- sensible defaults are filled in and shown as such ("We filled these in from the devices you
  picked. 29 of 36 set."), with an undo;
- closing asks "Discard changes?" only when something changed; the flow closes only after the
  Hub accepted the save.

**Settings page** (a device's settings, MQTT, remote settings):
- section labels over list groups; a Save bar fixed at the bottom, disabled until something
  changed, showing the first problem above the button;
- leaving with unsaved changes asks; switches that act at once (a toggle that needs no other
  field) save at once and say so in a snackbar.

**Saves:** the Hub is the only source of truth. A save shows progress, then re-reads what the
Hub stored; a refused save keeps the user's input and shows the Hub's message. If the
configuration changed elsewhere meanwhile, the app says so and offers to reload, never
overwrites silently.

**Destructive actions** (delete, unpair, restore, factory reset, reboot) confirm in a dialog
that names the thing ("Delete TV Living?") and what else it affects ("Used by 2 activities").
Ordinary sends never confirm.

**Long operations** (update, RF update of a remote, backup restore) show a stepped meter, can
be left and returned to, and say plainly when the room will be interrupted.

### 10.3 Errors

- An error says what happened and what to do, in the user's language, with the Hub's own
  message available under "Details".
- No `alert()`, no raw JSON, no error that disappears before it can be read.
- Signed out or token expired: the sign-in card, then back to the same screen.
- A role that cannot do something does not see the control, or sees it disabled with "Admin
  only".

## 11. Motion

- Easing: `cubic-bezier(0.2, 0, 0, 1)` for most changes; `cubic-bezier(0.05, 0.7, 0.1, 1)`
  for sheets and screen changes.
- Durations: 150 ms (press, fade), 250 ms (sheet, menu), 400 ms (screen change on large
  windows).
- Only transform, opacity and colour are animated. Screens fade up by 6 px, sheets rise, large
  dialogs scale from 96%.
- With "reduce motion" set, all of it is off.

## 12. Accessibility

- Contrast as in 3.1; focus is always visible (a two-layer ring: canvas, then primary).
- Every control is a real button, link or input with a name; icons alone have labels; state
  chips are read as text.
- Sheets and dialogs trap focus and return it on close; Escape closes them.
- Lists and the keypad are fully usable by keyboard and by a screen reader; the keypad is one
  group with named keys.
- Nothing depends on colour alone, hover alone, or a gesture with no button alternative.
- Touch targets as in section 5; text scales as in section 4.

## 13. Languages

- Six languages today (English, German, Spanish, French, Italian, Dutch); every string goes
  through the language files, including this design's new ones. No text in images.
- One term list for the whole app (Activity, Device, Command, Input, Remote, Hub, Turn off,
  Fix it), translated once and reused. Command names (`PowerOn`, `InputHdmi2`) are data, shown
  as they are.
- Layouts tolerate strings 30% longer and plural forms; numbers, dates and times follow the
  language.
- Language follows the browser or system, with an override in Settings, App.

## 14. Icons

One outline icon set with a matching filled variant for the selected tab, under a licence that
allows bundling (the set is chosen with upstream; Material Symbols is the default candidate),
inlined as SVG paths. 24 px in rows and bars, 20 px in buttons. No emoji as icons: they differ
per platform and cannot be themed. Activity and device icons come from the existing icon
picker, drawn in the same tile.

## 15. Platforms

| Platform | Form | Adds |
|---|---|---|
| Web | served by the Hub, any browser | nothing to install; "Add to home screen" icon |
| Android | today the web app in a native shell; the design allows a native app later without changing a screen | Hub discovery, haptics, hardware volume keys as VOL, and by priority: Quick Settings tile (Turn off), home-screen widget (activities), notification and lock-screen remote, deep links (`harmonyng://activity/<id>`) |
| Desktop (macOS first, then Windows and Linux) | the same web app in a window | Hub discovery, menu-bar or tray item with the activities and Turn off, global media keys, deploy and update of the Hub from the app |
| Physical remotes | their own on-device interface (`REMOTE.md`) | shares names, icons, activity order and state words with the apps; not otherwise covered here |

A feature exists in the web app first unless the platform makes it impossible there; a wrapped
app never has a different way of doing the same thing.

## 16. Budget

- Everything the web app needs is in the Hub's image: one page, one script, one stylesheet,
  icons inline. Target: under 250 KB compressed for the whole app, measured at each release.
- Views load on demand; the Remote tab is usable before the Settings code is loaded.
- No request to any host other than the Hub while drawing a screen. Features that need the
  internet (code database search, plugin marketplace) say so and fetch only when used.
- The app works with the Hub at 100 ms round trip and on a phone from 2018.

## 17. Moving from today's apps

1. Tokens and base components (section 3 to 5, 8) in the new web UI, old token names aliased;
   emoji replaced by icons.
2. Navigation: four tabs, routes, sub-page top bar; today's sections moved as in 6.3 without
   changing what they do.
3. Remote tab (7.1 to 7.3) with the state strip, More keys, Switch activity, Fix it.
4. Activities tab and the five-step editor; buttons editor.
5. Devices tab: list with badges, device page, add device, device settings, tools.
6. Settings pages, one by one, to the list and Save-bar pattern; Remotes first.
7. Light theme, accessibility pass, language pass.
8. Wrapped apps: connect screen, then the platform extras of section 15.

The legacy web UI stays available until step 6 is complete, then is retired.

## 18. Do and don't

**Do**
- open on what is running, with Turn off one tap away;
- show the command behind every key and the state behind every device;
- keep every function reachable from the four tabs in three taps or fewer;
- write new strings in all six languages in the same change.

**Don't**
- add a colour, radius or type size outside the tokens;
- use primary for power or delete, or green for anything but running and connected;
- confirm ordinary sends, or fail to confirm destructive ones;
- show "Off" for a device the Hub knows nothing about;
- load anything from the internet to draw a screen.

## 19. Open questions

- Native Android app or the wrapped web app with native extras: the tile, widget and
  lock-screen remote need native code either way.
- Desktop shell: Electron is the simple route and costs about 250 MB installed; a system
  web-view shell is far smaller but differs per platform.
- Icon set and its licence, to settle with upstream before step 1.
- Whether the light theme is wanted in the first release or after step 6.
- Which of today's two web UIs is the base. This document assumes the new one (`frontend/`).
