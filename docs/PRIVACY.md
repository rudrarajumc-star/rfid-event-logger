# Privacy

Applies to both SPS and LAI modes. Read this before running either
mode anywhere beyond a supervised demo.

## What this system does and doesn't identify

An RC522 reader confirms a specific RFID card was tapped. It does not
confirm who was holding it. Treat every event this system logs as "this
card was present," not "this person was present" — the gap between
those two is where card-sharing, lost/borrowed cards, and mistaken
identity all live. This repo does not claim to solve that gap, and
nothing in the dashboard or exports should be described as biometric or
identity-verified attendance.

## What's collected

Per event: an anonymous user code (`TUTOR-001` or, since
`v2.1.0-student-subject`, `STUDENT-001`), role, an anonymous card alias
(`CARD-T1`), timestamp, site/program code, event type, session id,
duration, and status. See `docs/LAI_MODE.md` for the full schema.
Student check-in adds one thing to that list: a self-selected subject
(MATH/READING/SCIENCE/GENERAL/ADMIN), picked by the student on the
dashboard and logged as its own event — still no name, still no
academic detail beyond which subject.

## What's deliberately never collected

Names, phone numbers, email addresses, physical addresses, photos,
student names, grades, academic records, disability status, or any
other identifying detail. There's no field for any of it — this isn't a
policy promise layered on top of a system that technically could
collect it, the schema itself doesn't have anywhere to put it.

## Where the real-identity mapping lives

The only place a card's real owner is knowable is `config/users_private.h`
on the specific physical device that was flashed with it — a local file,
gitignored, never committed. Revoking someone's access means setting
`active = false` on their row in that file; their anonymous code and
past attendance records are unaffected and unaffected records are never
deleted.

## Known gap: pre-anonymization git history (SPS side)

The SPS side of this repo's git history contains a small number of
early commits with real card UIDs and codes, predating an anonymization
pass — disclosed in the SPS README's Known Limitations rather than
hidden. An RFID UID isn't a secret (any nearby reader can read it), so
this isn't treated as a credential leak, but it's a real gap between
"anonymized in the current files" and "never existed in history."
Rewriting git history to remove it is possible (`git filter-repo` or
BFG) but hasn't been done. The LAI side of this repo starts clean —
`config/users_private.h` was never committed even once, so there's no
equivalent history to clean up here, provided that discipline holds for
every future commit.

## Security minimums before any real (non-demo) deployment

- The dashboard and CSV/hours exports have **no authentication**. Fine
  for a supervised demo on a network you control. Not fine for an
  unsupervised pilot, especially not on an open or shared network.
- Never expose the device to the public internet.
- Use a dedicated or access-controlled Wi-Fi network for a real pilot.
- Keep the device physically supervised during any real deployment.
- Clear test/demo records before and after a demonstration — don't let
  synthetic test data (like the `TUTOR-001`/`TUTOR-002` evidence in
  `docs/evidence/lai/`) linger on a device that later goes into real
  use.
- `config/organization.h` and `config/users_private.h` never leave the
  device they're flashed to — no cloud sync of those two files.
- Decide, before a pilot, who is allowed to download CSV/hours exports
  and where those downloaded copies are allowed to live.

## What a real pilot needs before it starts

A written approval covering purpose, what's collected, who
participates, confirmation that no minors are included in this first
phase, who can view exports, where records live, retention period, the
correction process for errors, who has authority to stop the pilot, and
confirmation that any public-facing evidence uses anonymized data only.
See `docs/PILOT_PROTOCOL.md`. The person leading this project should
not be the person who approves it for use on real people — that
approval needs to come from someone else with actual authority over the
program.
