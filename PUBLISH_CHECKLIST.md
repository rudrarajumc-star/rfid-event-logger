# Publish Checklist

Everything below is the final, honest state of this project as of the last
verification pass. Read it once before you push — it's short.

## 1. The one thing that can actually hurt you

The `.ino` file **on your Mac in Arduino IDE right now** (the one you've
been flashing to the device) has your real WiFi SSID and password typed
into it, because you typed them in yourself for testing. That is fine for
local testing. It is **not** fine to commit to GitHub.

**Do not** zip up or `git add` the live working folder from Arduino IDE
directly. Use the copy in this package instead — `rfid_event_logger.ino`
in this same folder has `WIFI_SSID = ""` and `WIFI_PASSWORD = ""` already
blanked out. That's the one to commit.

If you ever do accidentally commit the real credentials, deleting the
line in a later commit is not enough — the old commit still has it in
history forever unless you rewrite history (`git filter-repo` or BFG) or
just change your WiFi password afterward. Simplest fix: just don't commit
the live file in the first place.

## 2. Exact commands to publish

From a terminal, in the folder containing this package (unzip
`rfid_event_logger_repo.zip` first if you haven't):

```bash
cd github_repo
git init
git add .
git status          # <-- look at this output before committing.
                     #     confirm rfid_event_logger.ino shows blank
                     #     WIFI_SSID/WIFI_PASSWORD if you open it, and
                     #     that nothing named secrets.h is listed.
git commit -m "Initial commit: RFID event logger v1.4"
```

Then on github.com: click **New repository**, name it (e.g.
`rfid-event-logger`), leave it empty (no README/license/gitignore — you
already have those), set it Public if you want it linkable in
applications, then run the two commands GitHub shows you on the next
screen, which will look like:

```bash
git remote add origin https://github.com/<your-username>/<repo-name>.git
git branch -M main
git push -u origin main
```

You'll be prompted to authenticate — use a GitHub personal access token
or the GitHub CLI (`gh auth login`) if you have it set up, not your
account password (GitHub stopped accepting plain passwords for git
operations years ago).

## 3. Before you hit push — the copyright name

`LICENSE` currently says `Copyright (c) 2026 Rudra`. If you want your
full legal name there instead (more standard for something you might
link in a college application), open `LICENSE` and change that one line
before committing.

## 4. Optional but worth it: screenshots

`docs/screenshots/` exists but is empty (just a `.gitkeep` placeholder).
Drag in 2-3 real images before or after your first push — a phone photo
of the wired breadboard, a screenshot of the dashboard in a browser, a
screenshot of Serial Monitor output — then uncomment/add the image links
in the README's Screenshots section. This is the single highest-value
five minutes you can spend on this repo; a real photo of real hardware
does more for credibility than any more text will.

## 5. Final feature status — the actual source of truth

**Verified on real hardware (safe to describe as working, because it was
watched working):**

- RFID known/unknown card detection
- Check-in / check-out toggle with computed session duration
- Serial event logging
- WiFi connect + NTP time sync
- Live web dashboard (`/`)
- CSV export (`/events.csv`)
- Fail-safe operation (confirmed with SD and LCD both absent)
- Duplicate-scan suppression (`duplicate_ignored`, 2s window)
- Short-session anomaly flag (`flagged_short_session`, 5s window)
- Dashboard overview stats (checked-in count, total events, color-coded
  status)

**Wired but not verified — do not describe as "working," describe as
"implemented, pending verification":**

- MicroSD card logging. The module is wired as of your last message, but
  the isolated test sketch was never actually run to completion in this
  session — the screen session was stopped before results came back. If
  you test it yourself later and it works, update the README's status
  table (`Pending` → `Verified`) and add the real Serial output showing a
  successful write, the same way every other feature in this README is
  backed by a real captured log.

**Not built, hardware doesn't exist yet:**

- The physical 16x2 LCD. Web dashboard is the documented stand-in.

**Explicitly out of scope, not implied anywhere in this repo:**

- No backend server, database, multi-device sync, or authentication
  layer. This is a single ESP32 with on-device state. If you build the
  larger networked version later, that's a new project with its own
  README, not a retroactive claim added to this one.
