# LAI Pilot Protocol

This document is the plan for taking LAI mode from "engineering-tested
with synthetic data" (done, see `docs/LAI_MODE.md` and
`docs/evidence/lai/`) to "actually piloted with real tutors" (not
started). It exists so that step never gets skipped or fudged.

**Nothing in this file has happened yet.** It's a protocol, not a
report. Don't copy phrasing like "piloted" or measured results from
here into an application or a README until Phase 2/3 below have
actually run and you have real numbers to put in the blanks.

## Phase 1 — synthetic-data validation (done)

Using 2–3 fake test users (`TUTOR-001`, `TUTOR-002`, `COORD-001`),
verify on real hardware:

- [x] Unknown card → `denied_unknown_card`
- [x] Tutor check-in → `success`
- [x] Tutor check-out → `success` (or `flagged_short_session` if fast)
- [x] Duplicate scan → `duplicate_ignored`
- [x] Two simultaneous tutors, independently tracked
- [ ] Revoked card → `inactive_user`
- [ ] Restart mid-session (does a check-in survive a reboot? — it
      currently does NOT, `sessions[]` is in-memory only; a reboot loses
      any open session with no record of it. Worth deciding whether
      that's acceptable for a pilot or needs a fix first.)
- [ ] Wi-Fi loss mid-session
- [ ] Missing SD card (fails gracefully in SPS mode already; re-verify
      for LAI mode specifically)
- [ ] `events.csv` export — done, see `docs/evidence/lai/`
- [ ] `hours.csv` export — done, see `docs/evidence/lai/`
- [ ] Ring-buffer overflow (>15 events since boot)

Wrong card / long-session flag / production 15-minute threshold have
not been separately tested in LAI mode yet either.

## Phase 2 — authorized controlled pilot (not started)

This phase cannot start without:

1. **Written approval from an actual LAI supervisor or coordinator who
   is not you.** You lead LAI — you cannot be the person who approves
   your own system for use on real people. See the approval-document
   template (delivered separately from this repo, since it's an
   internal decision document, not something to publish).
2. Confirmation that no minors are enrolled as tracked users in this
   phase. Tutor-only, adult/teen tutors who can personally consent.
3. A dedicated or controlled network — the device must not be exposed
   to the open internet.

Once approved, the pilot itself:

- One tutoring site, one device
- 3–5 consenting tutors, one coordinator
- Two or three real tutoring sessions
- Existing manual/paper attendance kept running in parallel — do not
  replace it yet. The RFID system's job in this phase is to be checked
  against the manual record, not to replace it.
- No student data of any kind enters this system in this phase.

## Phase 3 — evaluation (not started)

After Phase 2, record actual observed numbers — not estimates, not
round numbers that sound plausible:

```
Participants:
Sessions:
Total scan events:
Successful scans:
Denied (unknown card) scans:
Duplicates suppressed:
Short/long-session flags (and how many were reviewed and resolved):
Forgotten checkouts (and how each was corrected — see below):
System uptime / any crashes or reboots:
Agreement rate with the manual/paper record:
Coordinator feedback:
Tutor feedback:
```

Only report metrics that were genuinely observed and counted, the same
standard the rest of this repo already holds itself to (see
`docs/DEVELOPMENT_LOG.md`'s correction note on the short-session timing
evidence — the pattern here is "verify it actually happened before
writing it down," not "write down what it should say").

## Forgotten-checkout correction workflow

This firmware does not auto-close a session (see `docs/LAI_MODE.md` for
why forced_checkout / missing_checkout aren't implemented in-device).
The manual process:

1. Coordinator notices (via the dashboard's "possible forgotten
   checkout" warning, or at end-of-day) that a tutor is still shown
   Active with no matching checkout.
2. Export `events.csv`.
3. In a protected administrative spreadsheet (not the public repo, not
   the device), add a corrected row. Preserve, don't overwrite:
   ```
   original_event: <the check-in event_id, unchanged>
   corrected_value: <the checkout time the coordinator determined>
   reason: forgotten_checkout
   corrected_by: <coordinator name>
   correction_timestamp: <when the correction was made>
   status: manually_reviewed
   ```
4. The original exported CSV is never edited in place. The correction
   lives alongside it, not instead of it.

## Rollback / stop conditions

Any of the following stops the pilot immediately, no separate
escalation needed:

- A tutor or coordinator withdraws consent
- Real names, contact info, or student data end up in an export or the
  dashboard by accident
- The device is exposed to a network it shouldn't be on
- The coordinator or LAI leadership simply wants to stop
