# LAI mode — synthetic test evidence

Captured on real hardware (the same ESP32/RC522/breadboard rig used for
the SPS-mode testing), flashed with `ACTIVE_MODE = LAI_MODE` and two of
the project's existing physical test cards re-registered under synthetic
tutor identities (`TUTOR-001` / `CARD-T1`, `TUTOR-002` / `CARD-T2`) — see
`config/users_private.h` (not committed; local to the test device).

**This is a synthetic-data engineering test, not an LAI pilot.** No real
tutor, real card, or real LAI site was involved. It demonstrates the
dual-mode firmware works, nothing about whether LAI wants to use it.

## What was tested and observed

1. Device boot in LAI mode — dashboard title ("LAI Tutor Attendance"),
   subtitle ("Volunteer Hour Verification"), and site/program code
   ("SITE-01 · GENERAL") all rendered correctly, confirmed live in the
   browser at the time of the test. No screenshot or raw boot-log text
   capture is included in this folder — only the two CSV exports below,
   which are the actual load-bearing evidence and are easier to verify
   than an image.
2. Tutor check-in (`TUTOR-001`, then `TUTOR-002`) — both correctly
   logged `success`, both appeared in the dashboard's active-tutor table
   simultaneously with independent elapsed-time counters.
3. Duplicate tap suppression — a repeat tap of the same card within 2s
   correctly logged `duplicate_ignored` and did not toggle session state,
   for both test cards.
4. Normal checkout with computed duration — `TUTOR-001` checked out
   after 17s, correctly logged `success` (session was long enough to
   clear the 8-second *testing* threshold used for this run — see the
   note below on production thresholds).
5. `events.csv` export — downloaded from `/events.csv`, all 14 schema
   fields populated correctly. See `events_synthetic_test.csv` in this
   folder.
6. `hours.csv` export — downloaded from `/hours.csv`, correctly
   aggregated per-tutor verified minutes and correctly labeled its
   `source` as `ring_buffer_since_boot_only` (no SD card was present for
   this test run, so the export honestly reports it only covers events
   since the last boot — see `handleHoursExport()` in the firmware for
   why this matters).

## Student check-in + subject selection (v2.1.0-student-subject)

A second test, after the firmware gained a `student` role and one-click
subject selection: `CARD-S1` (`STUDENT-001`, the card that was
`TUTOR-002` in the first test) checked in, immediately logged with
`program_code = PENDING`; a subject link on the dashboard was clicked
(READING), which logged a separate `subject-selected` event rather than
rewriting the check-in row — same append-don't-overwrite pattern as the
rest of this project. `CARD-T1` (`TUTOR-001`) checked in at the same
time using the device's default `GENERAL` program code, confirming
tutor and student roles coexist correctly on one device. See
`events_student_subject_test.csv` in this folder. This run happened to
have `wifiTimeAvailable = false` (NTP hadn't synced that boot), so
timestamps show the `T+Ns` relative-time fallback and `session_id` uses
the `00000000` date placeholder — both exactly the documented fallback
behavior, not a bug, and incidentally good evidence that the fallback
path also works under the new schema.

**Not tested in this run:** checkout was not captured for either user in
this pass — only check-in and subject-selection. A student checkout
(and the resulting `flagged_short_session`/`success` status on a
`program_code` other than the device default) hasn't been separately
verified yet.

## What was NOT tested in this run

- `flagged_short_session` / `flagged_long_session` in LAI mode
  specifically (verified in SPS mode with the millisecond-precision test
  in `docs/DEVELOPMENT_LOG.md`; the underlying logic is
  organization-independent, but a dedicated LAI-mode short-session test
  hasn't been captured yet).
- `inactive_user` (revoked card) — not exercised this run.
- MicroSD logging in LAI mode (SD has never been verified writing in
  this project — see the SPS README's Known Limitations).
- Anything involving more than 2 simultaneous users, a second physical
  site, or a multi-day span.

## Production threshold reminder

This test used `minValidSessionMs = 8000` (8 seconds) from the *testing*
example config, not the 15-minute production value LAI leadership would
need to approve. Don't ship this test threshold to a real pilot device —
see `config/organization.example.h` and `docs/LAI_MODE.md`.
