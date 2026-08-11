# Publish Checklist

This file is updated each time something meaningfully changes — it's
the current state, not a one-time pre-launch snapshot. If a section
below stops matching reality, fix the section, don't leave it stale.

## 1. The one thing that can actually hurt you

The `.ino` file **on your Mac in Arduino IDE right now** (the one
you've been flashing to the device) has your real WiFi SSID and
password typed into it. That's fine for local testing. It is **not**
fine to commit to GitHub.

**Do not** `git add` the live working folder from Arduino IDE directly.
The repo copy at `firmware/rfid_event_logger/rfid_event_logger.ino` has
`WIFI_SSID = ""` and `WIFI_PASSWORD = ""` blanked out — that's the one
to commit. Before every commit, actually open that file and check those
two lines yourself rather than trusting that it's still blank from last
time.

If real credentials ever do land in a commit, deleting the line in a
later commit is not enough — the old commit still has it in history
forever unless you rewrite history (`git filter-repo` or BFG) or just
change your WiFi password. Simplest fix: don't commit the live file in
the first place.

Same rule for `config/organization.h` and `config/users_private.h`
under `firmware/rfid_event_logger/config/` — both gitignored, neither
should ever show up in `git status`. If one does, stop and fix
`.gitignore` before committing, don't just `git add` past it.

## 2. Repo structure (why it matters)

```
firmware/rfid_event_logger/rfid_event_logger.ino   <- the sketch
firmware/rfid_event_logger/config/*.example.h      <- public templates
firmware/rfid_event_logger/config/{organization,users_private}.h  <- gitignored, real
docs/                                               <- everything else
```

The firmware's containing folder is named `rfid_event_logger` to match
the `.ino` filename exactly — Arduino IDE and `arduino-cli` both
require this to recognize a valid sketch. Don't flatten this back to
the repo root or rename either one independently of the other.

## 3. Exact commands to publish a change

```bash
cd ~/Downloads/sps-rfid-attendance   # or wherever you cloned it
git add .
git status
# Look at this output before committing, every time:
#   - firmware/rfid_event_logger/config/organization.h must NOT appear
#   - firmware/rfid_event_logger/config/users_private.h must NOT appear
#   - secrets.h must NOT appear
#   - open rfid_event_logger.ino and confirm WIFI_SSID/WIFI_PASSWORD are ""
git commit -m "<describe what actually changed>"
git push
```

First-time setup only (repo already exists on GitHub at this point):

```bash
git remote add origin https://github.com/<your-username>/<repo-name>.git
git branch -M main
git push -u origin main
```

Authenticate with a GitHub personal access token or `gh auth login`,
not your account password.

## 4. Copyright name

`LICENSE` currently says `Copyright (c) 2026 Rudra`. If you want your
full legal name there instead (more standard for something linked in a
college application), change that one line.

## 5. Screenshots and evidence

SPS mode: `docs/screenshots/wiring.png` and `docs/screenshots/dashboard.png`
are real photos, already in the repo and linked from the README. A
Serial Monitor screenshot is still outstanding there.

LAI mode: no screenshots — real CSV exports instead, in
`docs/evidence/lai/`, which are easier to verify than an image and
don't risk leaking anything if a screenshot happened to catch something
in the background. Don't add a LAI dashboard screenshot without
checking it doesn't expose real data first.

## 6. Feature status — the actual source of truth

This mirrors the README's status table; if the two ever disagree, the
README is what a visitor reads first, so keep it authoritative and fix
this file to match, not the other way around.

**Verified on real hardware:**

- RFID known/unknown card detection, check-in/check-out with duration
- Serial event logging, WiFi + NTP time sync
- Live web dashboard, `/events.csv` and `/hours.csv` exports
- Fail-safe operation (SD and LCD both absent)
- Duplicate-scan suppression, short-session flag (millisecond-precision
  re-test — see `docs/DEVELOPMENT_LOG.md`)
- Dual-mode config (SPS/LAI) — dashboard title, role label, site/program
  code, thresholds all confirmed switching correctly
- LAI-mode tutor check-in/check-out/duplicate-suppression, hours export
- Student check-in + one-click subject selection (check-in and
  subject-selected confirmed live; **checkout not yet captured for a
  student session** — see `docs/evidence/lai/`)

**Wired but not verified:**

- MicroSD card logging — still never watched actually writing, in
  either org mode.

**Not built:**

- Physical 16x2 LCD (web dashboard is the documented stand-in).

**Explicitly out of scope everywhere in this repo:**

- No backend server, database, multi-device sync, or dashboard/export
  authentication.
- No real LAI (or SPS) pilot has happened. Everything LAI-related is
  engineering-verified with synthetic data only — see
  `docs/PILOT_PROTOCOL.md` for what would actually need to happen
  before that changes, and don't describe this project as "piloted"
  anywhere until it has.
