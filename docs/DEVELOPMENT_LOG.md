# Development Log

Chronological record of what was built, what broke, and how it was
diagnosed and fixed. Kept separate from the README so the README can stay
focused on "how to use this," while this stays the honest paper trail of
what actually happened.

## v1.0 — Initial RFID read

Basic RC522 + ESP32 wiring, reading card UIDs to Serial.

**Bug: RC522 not responding (version register read `0xFF`).** The RC522's
power LED was lit, ruling out a power problem and narrowing the fault to
the four SPI signal wires. Root cause: the SDA/SS line was physically
wired to ESP32 pin D21 instead of D5 — a mislabeled-pin mistake, since D21
was intended for the LCD's I2C SDA line in this project, not the RFID
reader's SPI SS line. Diagnosed with a minimal, isolated test sketch
(just SPI.begin() + PCD_Init() + a version-register read) to rule out
everything else in the full sketch. Moving the wire to D5 fixed it
immediately — version register then read `0x82`, and UID reads succeeded.

## v1.1–v1.2 — Session tracking, LCD, MicroSD

Added known-user check-in/check-out logic, a 16x2 I2C LCD for status
display, and MicroSD CSV backup logging. Both the LCD and SD card were
made optional via graceful-degradation flags (`lcdAvailable`,
`sdAvailable`) so a missing or failed component never blocks the core
scan pipeline.

**Bug: RC522 stopped responding after adding the SD card.** After wiring
in the MicroSD module (which shares the SPI bus — SCK/MOSI/MISO — with the
RC522, using a separate CS pin), tapping a card produced zero Serial
output in the full sketch, despite the RC522 reading correctly moments
earlier in isolation. Root cause: `SD.begin()` (inside `initSD()`) can
leave the shared SPI bus in a different state, leaving the RC522
unresponsive even though its own wiring was completely untouched. Fixed
by calling `rfid.PCD_Init()` again immediately after `initSD()` in
`setup()`, forcing the RC522 back to a known-good register/SPI state
regardless of what `SD.begin()` did to the shared bus. Verified fixed via
real card taps producing correct CSV log lines.

At this point the full pipeline was verified end-to-end on real hardware:
real card taps produced real log lines, e.g.:

```
T+70s,SPS,unknown,-,6E1F4E06,scan,,denied
T+74s,SPS,unknown,-,21130407,scan,,denied
```

## v1.3 — Web dashboard, CSV export, fail-safe framing

Added a live web dashboard (`WebServer.h`, bundled with the ESP32 board
package — no extra library install), a `/events.csv` export endpoint, and
an in-memory ring buffer (`recentEvents[]`) so the dashboard and CSV
export have data to show even with no SD card present. This was
explicitly built as a stand-in for the physical LCD status display, which
hadn't arrived yet at the time.

The fail-safe design was made explicit: `wifiConnected`, `sdAvailable`,
and `lcdAvailable` each independently gate their subsystem, so any one
failing never blocks the core scan/check-in/check-out/log pipeline.

**Caught before upload: a typo in the UID edit.** While registering a
real card's UID into `knownUsers[]` by editing `{0xA4, 0x7B, 0x91, 0x2C}`
to the real UID, the edit left a duplicated prefix behind, producing
`{0x0x21, 0x13, 0x04, 0x07}` — invalid syntax that would have failed to
compile. Caught by re-reading the line before uploading, and fixed via
find/replace before flashing. Mentioned here because it's a real example
of double-checking an edit against the actual file content rather than
assuming a change landed correctly.

Verified end-to-end on real hardware:

- WiFi connect + NTP sync: `WiFi connected. IP address: 192.168.1.30` /
  `WiFi + NTP time synced.`
- Dashboard boot line: `Dashboard running at http://192.168.1.30/`
- Unregistered-card denial, 4 real taps across 2 physical cards, both
  correctly flagged `unknown` / `denied`
- Dashboard page loaded in a real browser on the same network, confirmed
  rendering the same 4 events live, most-recent-first
- CSV export confirmed via a real browser download of `attendance.csv`
- Fail-safe confirmed live: SD and LCD were both absent/off for this
  entire test run, and WiFi/dashboard/CSV all worked correctly anyway
- One real card (`21130407`) registered as `SPS-001` in `knownUsers[]`,
  re-flashed, and the full check-in → check-out → check-in cycle
  confirmed via real Serial output with correct `success` status and a
  computed duration on check-out

## v1.4 — Duplicate-scan suppression, short-session anomaly flag, dashboard stats

Added data-quality protections on top of the working v1.3 pipeline: a
per-user `Session` struct (`checkedIn`, `checkInMillis`, `lastScanMillis`),
`isDuplicateScan()` to ignore accidental repeat taps of the same card
within `DUPLICATE_WINDOW_MS`, and a `flagged_short_session` status on any
check-out happening within `SHORT_SESSION_MS` of its check-in. Also added
a `totalEventsLogged` counter and dashboard overview stat pills
(`Checked In: N`, `Total Events: N`), plus per-row color coding on the
dashboard event table (green/amber/red by status).

**Bug caught before live testing: overlapping time windows made the
anomaly flag unreachable.** Both `DUPLICATE_WINDOW_MS` and
`SHORT_SESSION_MS` were initially set to 5000ms. Since `isDuplicateScan()`
runs first in `loop()`, any tap within 5s of the previous scan on the same
card — including a legitimate fast check-out — would always be caught and
logged as `duplicate_ignored` before `handleKnownScan()` ever got a chance
to evaluate the short-session condition, making `flagged_short_session`
dead code. Caught by re-reading the logic before flashing, not by a
compiler or runtime error. Fixed by narrowing `DUPLICATE_WINDOW_MS` to
2000ms so the two windows no longer fully overlap: a repeat tap under 2s
is a duplicate, a check-out between 2s and 5s after check-in is a flagged
short session, anything else is normal. Verified live afterward that both
branches are independently reachable (see evidence below).

**Caught before upload: a second stray-edit typo.** While editing the
`DUPLICATE_WINDOW_MS` comment, the edit corrupted `"...within 2s"` into
`"...within 2hin 5s"`. Caught by re-reading the editor state before
compiling. Fixed by selecting the full line and replacing it cleanly
rather than patching around the typo — same failure mode as the v1.3 UID
typo, same fix pattern.

**Upload error: `exit status 2`.** One upload attempt after the threshold
fix failed with a generic exit-status-2 error from the ESP32 upload tool,
most likely transient serial port contention. Fixed by simply retrying
the upload with no other changes — the second attempt succeeded cleanly.

**Ambiguous evidence on the first test pass.** The first attempt to
trigger and observe `duplicate_ignored` produced a Serial Monitor line
that was visually merged with boot-noise garbling, making it impossible
to confirm with certainty that the duplicate-suppression branch had
actually fired rather than a normal scan. Rather than record this as
verified on ambiguous evidence, a second, deliberate test was run: the
same card tapped twice, back-to-back as fast as physically possible. This
produced an unambiguous
`...,scan,,duplicate_ignored` line, resolving the ambiguity.

Verified end-to-end on real hardware, using real card `21130407`
(`SPS-001`):

```
2026-08-03 17:58:40,SPS,employee,SPS-001,21130407,check-in,,success
2026-08-03 17:58:41,SPS,employee,SPS-001,21130407,check-out,0,flagged_short_session
2026-08-03 17:58:43,SPS,employee,SPS-001,21130407,scan,,duplicate_ignored
2026-08-03 17:58:46,SPS,employee,SPS-001,21130407,check-out,0,success
```

- Check-in → check-out under 5s: correctly flagged `flagged_short_session`
  instead of silently logged as `success`.
- Rapid repeat tap within 2s: correctly ignored as `duplicate_ignored`,
  confirmed it did **not** toggle check-in/check-out state a second time.
- A later, deliberate check-out outside both windows: correctly logged
  as plain `success` with a computed duration.
- Dashboard reloaded in a real browser after this sequence, confirmed
  showing `Checked In: 1`, `Total Events: 9`, and the event table
  color-coded per status (green/amber/red).

## What's still unverified

- MicroSD card actually writing to a physical card (`sdAvailable` has
  read `false`/`OFF` throughout every test run so far — either no card is
  currently inserted, or the card/module needs further debugging).
- The physical 16x2 LCD — hardware hadn't arrived yet as of this log.
- The duplicate/short-session thresholds (2s/5s) are reasonable starting
  values for a hallway badge-tap use case but haven't been tuned against
  real usage patterns over time.
- No backend, database, multi-device sync, or auth layer exists. This
  project is a single-device, on-device-state logger; a networked,
  offline-first, multi-device system with a real operational pilot would
  be a separate, multi-day follow-on project, not an extension of this
  session's scope.
