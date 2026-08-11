# LAI Mode

This document describes the `LAI_MODE` configuration of the RFID Event
Logger: tutor attendance and volunteer-hour verification for Learning
Access Initiative (LAI) tutoring sites, built on the same firmware as the
SPS workforce-attendance deployment.

**Status: engineering complete, not yet piloted.** Everything below that
says "implemented" or "verified" means exactly that — implemented and
tested with synthetic data on real hardware. It does not mean an actual
LAI pilot with real tutors has happened. See the status table at the
bottom and `PILOT_PROTOCOL.md` for what that would actually require.

## The problem

LAI needed a way to answer, per tutoring site:

- Which tutor arrived, and when did they start/end?
- How many verified volunteer hours did they complete?
- Which program/session did they support?
- Were there duplicate or suspicious scans?
- Can a coordinator export records and see hours summarized by tutor?

That's the scope. This is explicitly **not**: student academic records,
parent accounts, background checks, payments, automatic school-system
integration, biometric identity, or a guarantee against card-sharing. An
RC522 reader confirms a card was present — it does not confirm who was
holding it.

## Users: tutor cards, and optionally student check-in

The base version issues cards to consenting adult/teen tutors, each
tied to an anonymous code (`TUTOR-001`) rather than a name.

As of `v2.1.0-student-subject`, a `role == "student"` card is also
supported, using the same anonymous-code discipline (`STUDENT-001`,
never a name). A student check-in doesn't just toggle Active — the
dashboard shows one-click subject links (MATH/READING/SCIENCE/GENERAL/
ADMIN) next to that student's row; picking one logs a separate
`subject-selected` event rather than editing the check-in row. See
`docs/evidence/lai/` for a real-hardware test of this flow.

**This is a bigger privacy commitment than tutor-only tracking, even
though nothing identifying is collected.** Read `docs/PRIVACY.md` and
use the updated pilot-approval template before this is ever pointed at
a real student, even under an anonymous code — "no minors tracked" is
no longer automatically true once this feature is actually in use, and
that question needs an honest real answer from whoever approves a
pilot, not an assumption carried over from the tutor-only version.

## Configuration model

One physical device runs one `OrganizationConfig` — see
`config/organization.example.h`. Switching a device between SPS and LAI
is a one-line change (`ACTIVE_MODE`) plus a re-upload; there's no
runtime toggle. The config controls the dashboard title/subtitle, the
role label ("Employee" vs "Tutor"), whether a site code is required,
and the short/long-session thresholds:

```cpp
const OrganizationConfig LAI_CONFIG = {
  "LAI", "LAI Tutor Attendance", "Volunteer Hour Verification", "Tutor",
  true,                        // requires a site code
  15UL * 60UL * 1000UL,        // production: 15-minute minimum valid session
  5UL * 60UL * 60UL * 1000UL   // 5-hour long-session warning
};
```

The 15-minute production threshold needs sign-off from LAI leadership
before a real pilot — the shipped example file uses a much shorter
testing value so you can actually exercise the logic without standing
around for 15 real minutes per test run. Don't ship the short value to
a real pilot device.

## Data model

Each registered tutor:

```cpp
struct User {
  byte uid[4];
  const char* userCode;   // "TUTOR-001" — stable across card replacement
  const char* role;       // "tutor", "coordinator"
  const char* cardAlias;  // "CARD-T1" — logged instead of the raw UID
  bool active;            // false = revoked, history is kept
};
```

Every logged event (14 fields, one consistent schema across both org
modes):

```
event_id,timestamp,organization,site_code,program_code,user_code,role,
card_alias,event_type,session_id,duration_minutes,status,device_id,
firmware_version
```

Example:

```csv
E00041,2026-08-15 10:02:11,LAI,SITE-01,GENERAL,TUTOR-001,tutor,CARD-T1,check-in,S-20260815-001,,success,LAI-READER-01,v2.0.0-dual-mode
E00042,2026-08-15 11:31:48,LAI,SITE-01,GENERAL,TUTOR-001,tutor,CARD-T1,check-out,S-20260815-001,89,success,LAI-READER-01,v2.0.0-dual-mode
```

No tutor names, phone numbers, emails, or any student data ever enter
this log — the firmware doesn't have a field for them.

## Statuses

Emitted live by this firmware:

| Status | Meaning |
|---|---|
| `success` | Normal check-in/check-out |
| `denied_unknown_card` | Card not in the registry |
| `duplicate_ignored` | Repeat tap of the same card within 2s |
| `flagged_short_session` | Check-out faster than the minimum valid session — still logged, still counts toward hours, flagged for human review |
| `flagged_long_session` | Check-out slower than the long-session warning — same treatment |
| `inactive_user` | Card is registered but revoked |

**Documented but not implemented by this single-device firmware** —
these describe a *manual correction workflow* an administrator runs
after the fact on an exported record, not something the device does
live:

| Status | Meaning | Where it actually happens |
|---|---|---|
| `forced_checkout` | Admin manually closed a session that never got a real checkout tap | Correction spreadsheet — see `PILOT_PROTOCOL.md` |
| `missing_checkout` | A session was left open (tutor forgot to tap out) | Same |
| `wrong_site` | Would apply to a multi-site deployment; this version is one device per site, so it can't currently fire | N/A until multi-site |

Implementing these for real would mean building an audit trail
(`original_event`, `corrected_value`, `reason`, `corrected_by`,
`correction_timestamp`) so a correction is trustworthy instead of a
silent overwrite. That's future work, not claimed as done here.

Student check-in also introduces a new `event_type` (not a status):
`subject-selected`, logged when a student picks their subject on the
dashboard. It always carries `status = success` — subject selection
isn't something that gets flagged, it's just a record that the choice
was made, when, and what it was.

## Dashboard

Header shows the org title/subtitle and, in LAI mode, the site and
program code. Below that: tutors currently checked in (with elapsed
time, and a "possible forgotten checkout" warning if someone's been
checked in longer than the long-session threshold), completed sessions
and verified minutes **since boot** (not calendar-day — see the honesty
note in the firmware's `handleHoursExport()`), and the recent-events
table with the same status color-coding as the SPS dashboard.

Exports: `/events.csv` (raw event log — SD file if present, else the
15-entry ring buffer since last boot) and `/hours.csv` (per-tutor
verified-minutes summary, same source-availability caveat, and the CSV
itself labels which source it came from in a `source` column so no one
downstream has to guess).

## Site and program codes

`DEVICE_ID`, `SITE_CODE`, `PROGRAM_CODE` are set per physical device in
the private `config/organization.h` (see `config/organization.example.h`
for the placeholder). For the initial pilot, that's one device, one
site, hard-coded — no multi-site routing logic exists yet.

## Registration / revocation

Issuing a card: confirm identity, get consent, tap the unregistered
card once (UID prints to Serial), copy that UID into
`config/users_private.h` with a new anonymous `userCode`/`cardAlias`,
set `active = true`, re-upload. Revoking: set `active = false` on that
row — never delete it, since past attendance records reference that
`userCode` and must stay valid. `config/users_private.h` is gitignored;
only `config/users.example.h` (fully synthetic UIDs) is public.

## Testing status

| LAI capability | Status |
|---|---|
| LAI dashboard configuration (title, labels, site/program display) | **Implemented** |
| Tutor roles and anonymous codes | **Implemented** |
| Hardware test with synthetic users (2 physical test cards as TUTOR-001/TUTOR-002) | **Verified** — see `docs/evidence/lai/` |
| Volunteer-hour CSV export | **Implemented**, verified against synthetic data only |
| Student check-in + one-click subject selection | **Implemented, partially verified** — check-in and subject-selected confirmed live; student checkout not yet captured. See `docs/evidence/lai/` |
| Authorized LAI pilot (tutors) | **Not started** |
| Authorized LAI pilot (students) | **Not started** — needs its own approval, see `docs/PRIVACY.md` |
| Real recurring deployment | **Not deployed** |

## Privacy limitations

See `docs/PRIVACY.md`. Short version: no authentication on the
dashboard/exports (fine for a supervised demo, not fine for an
unsupervised real pilot on an open network), RFID UID is a possession
check not an identity guarantee, and this repo's git history predates
an anonymization pass on the SPS side — the same discipline applies
here: nothing LAI-identifying goes into a public commit in the first
place, rather than being scrubbed after.
