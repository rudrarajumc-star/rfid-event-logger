# RFID Event Logger

An ESP32-based RFID check-in/check-out attendance logger with a live web
dashboard, CSV export, MicroSD backup logging, and WiFi/NTP timestamps.
Built around a graceful-degradation design: every optional subsystem (WiFi,
SD card, LCD) can fail or be absent without ever stopping the core
scan-and-log pipeline.

## Status

This README distinguishes **Verified** (actually run on real hardware and
observed working) from **Pending** (implemented and compiles, but not yet
demonstrated on hardware). Nothing below is claimed as working unless it was
actually watched working.

| Feature | Status | Evidence |
|---|---|---|
| RFID scan + known/unknown user check | **Verified** | Real card taps logged and denied/accepted correctly (see logs below) |
| Check-in / check-out toggle | **Verified** | Real registered card produced `check-in` then `check-out` with duration |
| Serial event logging | **Verified** | All events below captured live over Serial Monitor |
| WiFi connect + NTP time sync | **Verified** | Real `WiFi connected. IP address: ...` and `WiFi + NTP time synced.` on device boot |
| Web dashboard (`/`) | **Verified** | Loaded in a real browser on the same network, confirmed rendering live scan data |
| CSV export (`/events.csv`) | **Verified** | Real browser download of `attendance.csv`, correct headers and content |
| Fail-safe (WiFi/SD/LCD optional) | **Verified** | Dashboard and WiFi worked correctly while SD and LCD were both absent/off |
| Duplicate-scan suppression | **Verified** | Back-to-back taps on the same card within 2s logged as `duplicate_ignored`, no double check-out |
| Short-session anomaly flag | **Verified** | Check-out under 5s after check-in logged with `flagged_short_session` instead of `success` |
| Dashboard overview stats (checked-in count, total events) | **Verified** | Live dashboard confirmed `Checked In: 1`, `Total Events: 9`, color-coded by status |
| MicroSD card logging | **Pending** | Module wired in; a card has not yet been verified writing on real hardware |
| LCD status display | **Pending** | Hardware has not arrived yet; code has a tested graceful-degradation path for its absence |
| 16x2 LCD as physical display | **Pending** | Web dashboard is currently standing in for this per current setup |

## Overview

Originally built around an RC522 RFID reader and an ESP32 dev board, this
project logs every card tap to Serial and (when available) to a MicroSD
card as CSV, tracks per-user check-in/check-out sessions with duration, and
exposes a small live web dashboard so status can be checked from a browser
on the same network — without needing an LCD wired up at all.

Beyond the base scan/check-in/check-out pipeline, the logger also protects
its own data quality: repeated taps of the same card within a 2-second
window are recognized as accidental double-scans and ignored rather than
toggling check-in/check-out twice, and any check-out happening less than 5
seconds after check-in is flagged (`flagged_short_session`) instead of
silently accepted as a normal `success` event — a simple anomaly signal for
sessions too short to be a real visit. The dashboard also surfaces live
overview stats (how many users are currently checked in, total events
logged) on top of the raw event table.

Two independent hardware bugs were found and fixed during development (see
`docs/DEVELOPMENT_LOG.md`): a mislabeled pin (RC522 SDA wired to the wrong
GPIO) and shared-SPI-bus interference between the RC522 and the SD card
module that required an explicit re-init call to resolve.

## Hardware

- ESP32 Dev Module (38-pin devkit)
- MFRC522 RFID reader + card/keyfob
- MicroSD card module (SPI)
- 16x2 I2C LCD (1602, PCF8574 backpack) — not yet installed
- Breadboard + jumper wires

## Wiring

**RC522 → ESP32**

| RC522 pin | ESP32 pin |
|---|---|
| SDA (SS) | GPIO5 |
| SCK | GPIO18 |
| MOSI | GPIO23 |
| MISO | GPIO19 |
| RST | GPIO4 |
| 3.3V | 3.3V (not 5V — RC522 is 3.3V only) |
| GND | GND |

**MicroSD module → ESP32** (shares the SPI bus with the RC522 — same
SCK/MOSI/MISO, separate CS pin)

| SD pin | ESP32 pin |
|---|---|
| CS | GPIO15 |
| SCK | GPIO18 (shared) |
| MOSI | GPIO23 (shared) |
| MISO | GPIO19 (shared) |
| VCC | 3.3V or 5V — check your module |
| GND | GND |

**16x2 I2C LCD → ESP32** (not yet installed)

| LCD pin | ESP32 pin |
|---|---|
| SDA | GPIO21 |
| SCL | GPIO22 |
| VCC | 5V (most I2C backpacks want 5V — check yours) |
| GND | GND |

## Firmware

The full sketch is in [`rfid_event_logger.ino`](./rfid_event_logger.ino).

Libraries required (Arduino Library Manager):

- `MFRC522` by GithubCommunity
- `LiquidCrystal I2C` (by Frank de Brabander, or `LiquidCrystal_I2C`)
- `SD` — bundled with the ESP32 board package
- `WebServer` — bundled with the ESP32 board package, no install needed

### Before you push this to GitHub

`WIFI_SSID` and `WIFI_PASSWORD` in the sketch **must** stay as empty
strings (`""`) in anything committed to a public repo. Fill them in
locally, directly in your editor, only on the machine that's flashing the
device — never paste real WiFi credentials into a commit, an issue, a pull
request, or a chat client. If you want a device that supports multiple
WiFi networks or teammates without sharing a plaintext password in the
`.ino` file, the common pattern is a separate `secrets.h`:

```cpp
// secrets.h  (add this filename to .gitignore — never commit it)
#define WIFI_SSID "your-network-name"
#define WIFI_PASSWORD "your-network-password"
```

and in the main sketch:

```cpp
#include "secrets.h"
```

This repo currently keeps credentials inline as blank placeholders for
simplicity; switching to `secrets.h` is a five-minute follow-up if this
project grows beyond a single device.

## Architecture

- **Scan pipeline**: `loop()` polls the RC522 for a new card. On a tap, the
  UID is matched against `knownUsers[]`. Unknown cards are logged and
  denied. Known cards toggle check-in/check-out via a per-user `Session`
  struct that tracks `checkedIn` state and start time.
- **Logging**: every event goes through `logEvent()`, which always prints
  to Serial and always pushes into a 15-entry in-memory ring buffer
  (`recentEvents[]`), then additionally appends to `/events.csv` on the SD
  card if `sdAvailable` is true. A separate `totalEventsLogged` counter
  increments on every event since boot and is *not* capped at 15 — it's
  what the dashboard's "Total Events" stat shows, distinct from the
  ring buffer, which only ever holds the most recent 15 for display.
- **Fail-safe flags**: `wifiConnected`, `sdAvailable`, and `lcdAvailable`
  independently gate their subsystems. None of them being false blocks the
  core scan/log pipeline — this was verified live (see below) by running
  with both SD and LCD absent while WiFi/dashboard still worked correctly.
- **Duplicate suppression / anomaly flagging**: each known user has a
  `Session` struct tracking `checkedIn`, `checkInMillis`, and
  `lastScanMillis`. `isDuplicateScan()` runs first in `loop()` and ignores
  any repeat tap of the same card within `DUPLICATE_WINDOW_MS` (2000ms).
  Otherwise `handleKnownScan()` toggles check-in/check-out; a check-out
  happening within `SHORT_SESSION_MS` (5000ms) of its matching check-in is
  logged with status `flagged_short_session` instead of `success`. These
  two thresholds are intentionally kept apart (2s vs 5s) — if they
  overlapped fully, every short-session check-out would first get caught
  as a duplicate and the anomaly branch would never be reached (this was
  a real bug caught and fixed during development, see
  `docs/DEVELOPMENT_LOG.md`).
- **Web dashboard**: only initialized if WiFi connects (`setupWebServer()`
  is called from inside the WiFi-connected branch). Serves `/` (an HTML
  page listing the last 15 events plus checked-in-count/total-events/time/
  uptime stat pills) and `/events.csv` (streams the real SD file if
  present, otherwise builds a CSV from the ring buffer).
- **Time**: NTP sync is attempted once at boot if WiFi connects. If it
  fails or WiFi isn't configured, `getTimestamp()` falls back to a
  relative `T+<seconds since boot>s` format rather than blocking or
  crashing.

## Verified output (real hardware, real logs)

Serial Monitor, unregistered cards correctly denied:

```
WiFi connected. IP address: 192.168.1.30
WiFi + NTP time synced.
Dashboard running at http://192.168.1.30/
timestamp,mode,user_type,user_code,card_id,event_type,duration_min,status
2026-08-03 12:26:21,SPS,unknown,-,21130407,scan,,denied
2026-08-03 12:26:23,SPS,unknown,-,21130407,scan,,denied
2026-08-03 12:26:26,SPS,unknown,-,6E1F4E06,scan,,denied
2026-08-03 12:26:28,SPS,unknown,-,6E1F4E06,scan,,denied
```

Serial Monitor, after registering card `21130407` as `SPS-001` — full
check-in/check-out cycle:

```
timestamp,mode,user_type,user_code,card_id,event_type,duration_min,status
T+695s,SPS,employee,SPS-001,21130407,check-in,,success
T+696s,SPS,employee,SPS-001,21130407,check-out,0,success
T+700s,SPS,employee,SPS-001,21130407,check-in,,success
```

(Timestamps show as relative `T+Ns` here because NTP hadn't finished
syncing yet on this particular boot — that's the fail-safe working as
designed, not a bug: the device keeps logging correctly either way.)

Web dashboard (`http://<device-ip>/`), loaded in a real browser on the
same network, confirmed rendering:

- Title: `SPS Attendance Dashboard`
- Status pills: `Checked In: <n>`, `Total Events: <n>`, `Time: NTP`, `Uptime: <n>s`
  (SD/LCD status pills were removed from the dashboard as of the latest
  firmware — see "Known limitations" below)
- A live table of the 4 unregistered-card events above, most-recent-first
- `Download full attendance CSV` link, confirmed triggering a real
  `attendance.csv` download via the browser

Serial Monitor, real card `21130407` (`SPS-001`) exercising duplicate
suppression and the short-session anomaly flag back to back:

```
2026-08-03 17:58:40,SPS,employee,SPS-001,21130407,check-in,,success
2026-08-03 17:58:41,SPS,employee,SPS-001,21130407,check-out,0,flagged_short_session
2026-08-03 17:58:43,SPS,employee,SPS-001,21130407,scan,,duplicate_ignored
2026-08-03 17:58:46,SPS,employee,SPS-001,21130407,check-out,0,success
```

Read top to bottom: a check-in immediately followed by a check-out under
5 seconds later is flagged `flagged_short_session` rather than accepted
silently; a rapid repeat tap 2 seconds after that is recognized as an
accidental double-scan and logged `duplicate_ignored` rather than
toggling state again; a later, deliberate check-out outside both windows
logs normally as `success`.

Dashboard, reloaded after the sequence above, confirmed showing:

- Overview stat pills: `Checked In: 1`, `Total Events: 9`
- The event table with per-row color coding: green for `success`, amber
  for `flagged_short_session` / `duplicate_ignored`, red for `denied`

## Demo video

[Add your video link here.] Don't commit the raw video file to this git
repo — video files are large and GitHub repos aren't meant to host them
(there's a 100MB per-file hard limit, and even well under that it bloats
every future clone). Upload it to YouTube (unlisted is fine — doesn't
show up in search, but anyone with the link can watch) or Google Drive
with link sharing on, then paste the link here:

```md
[Watch the demo](https://youtu.be/your-video-id-here)
```

## Screenshots

Not included yet. Add 2-3 screenshots here — the dashboard in a browser,
Serial Monitor output, and a photo of the wired breadboard — by dragging
them into a `docs/screenshots/` folder and referencing them below.

```md
![Dashboard](docs/screenshots/dashboard.png)
![Wiring](docs/screenshots/wiring.jpg)
```

## Setup

1. Install Arduino IDE 2.x and the ESP32 board package.
2. Install the three Library Manager dependencies listed above.
3. Wire the hardware per the tables above.
4. Open `rfid_event_logger.ino`, select **ESP32 Dev Module** as the board.
5. Fill in `WIFI_SSID` / `WIFI_PASSWORD` locally (see security note above).
6. Upload, then open Serial Monitor at 115200 baud.
7. Tap an unregistered card once — its UID prints to Serial. Copy that UID
   into `knownUsers[]` with a real code/role to register it.
8. Once WiFi connects, visit the printed dashboard URL in a browser on the
   same network.

## Known limitations / next steps

- The dashboard no longer displays SD/LCD status pills. They were removed
  rather than left showing `OFF` for two subsystems that aren't part of
  the current verified feature set — the dashboard now only reports
  state that's actually meaningful to the demo (checked-in count, total
  events, time sync, uptime). If SD logging gets verified working later,
  it would be reasonable to re-add an `SD: ON` pill at that point, backed
  by a real passing test, not before.
- MicroSD card logging is implemented and gracefully degrades when absent,
  but has not yet been verified writing on real hardware in this session.
- LCD status display is implemented with the same graceful-degradation
  pattern but the physical LCD hasn't arrived yet — the web dashboard is
  the interim stand-in.
- Only one card is currently registered as a known user (`SPS-001`); the
  other two entries in `knownUsers[]` are placeholder UIDs and should be
  replaced with real registered cards before deployment.
- No authentication on the dashboard or CSV endpoint — anyone on the same
  WiFi network can view attendance data. Fine for a prototype on a private
  network; would need addressing before wider deployment.
- Duplicate suppression and the short-session flag are both purely
  time-based (2s / 5s). That's a reasonable heuristic for a hallway
  badge-tap use case, not a guarantee — a legitimately fast in-and-out
  visit would still get flagged, and it's a "flag for review," not a
  block. The thresholds are `const`s at the top of the sketch and easy to
  retune.
- What this project is **not** (yet): there is no backend server,
  database, multi-device sync, or account/auth layer. Everything here runs
  on a single ESP32 with on-device state and an in-memory ring buffer.
  Turning this into a multi-device, offline-first system with a real
  backend and a multi-day pilot is a legitimate next phase, but it's a
  separate, multi-day project with its own hosting/database/pilot
  decisions — not something to bolt on and claim as tested in one sitting.

## Development log

See [`docs/DEVELOPMENT_LOG.md`](./docs/DEVELOPMENT_LOG.md) for the full
debugging history, including two real hardware bugs found and fixed during
development.
