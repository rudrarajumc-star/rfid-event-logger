/*
  config/users.example.h

  Copy this file to config/users_private.h before compiling.
  config/users_private.h is gitignored — real card UIDs and the real-name
  mapping never touch git, even in history.

  Every UID below is synthetic (made up) — none of them match a real card.
  card_alias is what actually gets logged to CSV/Serial/dashboard, never
  the raw uid[] bytes. That mapping (which uid[] belongs to which alias)
  only ever lives in this local file, on the device that was flashed with
  it — not in any exported record.

  role == "student" is a distinct path from "tutor"/"coordinator"/
  "employee": a student check-in shows one-click subject-selection links
  on the dashboard instead of just toggling straight to "Active". See
  docs/LAI_MODE.md. Read docs/PRIVACY.md before using the student role
  with any real person, even under an anonymous code — it's a bigger
  privacy commitment than tutor-only tracking and needs its own
  authorization, not just a config change.

  Registering a real card (see docs/LAI_MODE.md "Registration workflow"):
    1. Tap the new card once — its real UID prints to Serial.
    2. Copy those 4 bytes into a new row below, in your LOCAL
       config/users_private.h, never in a commit.
    3. Assign it an anonymous userCode + cardAlias that reveal nothing
       about the person (TUTOR-004, not "Jane R."; STUDENT-002, not a name).
    4. Set active = true, re-upload.

  Revoking a card: set active = false. Do NOT delete the row — the row
  going inactive is itself part of the audit trail, and old attendance
  records referencing that userCode must stay valid.
*/

#ifndef USERS_H
#define USERS_H

struct User {
  byte uid[4];
  const char* userCode;   // stable identity across card replacements, e.g. "TUTOR-001"
  const char* role;       // "tutor", "coordinator", "student", "employee", ...
  const char* cardAlias;  // logged in place of the raw UID, e.g. "CARD-A"
  bool active;            // false = revoked; card is denied, history is kept
};

User knownUsers[] = {
  { {0x12, 0x34, 0x56, 0x78}, "TUTOR-001",   "tutor",       "CARD-T1", true },
  { {0x9A, 0xBC, 0xDE, 0xF0}, "TUTOR-002",   "tutor",       "CARD-T2", true },
  { {0x01, 0x02, 0x03, 0x04}, "COORD-001",   "coordinator", "CARD-C1", true },
  { {0x55, 0x66, 0x77, 0x88}, "STUDENT-001", "student",     "CARD-S1", true },
};
const int NUM_KNOWN_USERS = sizeof(knownUsers) / sizeof(knownUsers[0]);

#endif
