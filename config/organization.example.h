/*
  config/organization.example.h

  Copy this file to config/organization.h before compiling.
  config/organization.h is gitignored — it's where a physical device picks
  which organization it's running for. Nothing in this file is a secret by
  itself (mode/site codes aren't sensitive), but keeping the *active* choice
  out of git means the SPS unit and an LAI unit can each have their own
  local organization.h without merge conflicts or a risk of accidentally
  flashing one org's device with the other org's mode.

  One physical device = one mode. There's no runtime switch — you pick
  ACTIVE_MODE below, re-upload, and that's the entire device's identity
  until you change this file and re-upload again.
*/

#ifndef ORGANIZATION_H
#define ORGANIZATION_H

enum OrganizationMode {
  SPS_MODE,
  LAI_MODE
};

struct OrganizationConfig {
  const char* organizationCode;     // short code, goes in every CSV row
  const char* dashboardTitle;
  const char* dashboardSubtitle;    // "" if not needed
  const char* primaryUserLabel;     // "Employee", "Tutor", etc — display only
  bool requireSiteSelection;        // does this org care about site_code?
  unsigned long minValidSessionMs;  // check-out faster than this -> flagged_short_session
  unsigned long longSessionWarningMs; // still checked-in longer than this -> flagged on dashboard; 0 = disabled
};

const OrganizationConfig SPS_CONFIG = {
  "SPS",
  "SPS Workforce Attendance",
  "",
  "Employee",
  false,
  5000UL,        // 5s — matches the original v1.4 SHORT_SESSION_MS behavior
  0UL            // SPS doesn't use a long-session warning
};

const OrganizationConfig LAI_CONFIG = {
  "LAI",
  "LAI Tutor Attendance",
  "Volunteer Hour Verification",
  "Tutor",
  true,
  // PRODUCTION value should be 15UL * 60UL * 1000UL (15 minutes) — get
  // that approved by LAI leadership before a real pilot. Left short here
  // so you can actually test check-in/check-out logic without standing
  // around for 15 real minutes per test.
  8000UL,                          // TESTING value — swap for the real 15-min threshold before a pilot
  5UL * 60UL * 60UL * 1000UL        // 5 hours — flags a session that's still open, likely a forgotten checkout
};

// ---- Pick ONE active mode for THIS physical device ----
const OrganizationMode ACTIVE_MODE = LAI_MODE;

// ---- Per-device identity ----
// Only meaningful when requireSiteSelection is true (LAI), but always
// logged so every CSV row has a consistent schema regardless of org.
#define DEVICE_ID    "LAI-READER-01"
#define SITE_CODE    "SITE-01"
#define PROGRAM_CODE "GENERAL"

#endif
