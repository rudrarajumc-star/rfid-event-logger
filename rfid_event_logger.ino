/*
  Embedded RFID Event Logger — v2.1.0 (dual-mode: SPS / LAI, + student subject pick)
  ESP32 + RC522 (SPI) + 16x2 I2C LCD + MicroSD CSV backup + WiFi web dashboard
  (all graceful-degradation)

  What changed in v2.1.0 — a second card role, student check-in:
    - `role == "student"` is now a distinct path from tutor/coordinator.
      A student card check-in logs immediately (program_code =
      "PENDING") and the dashboard shows one-click subject links
      (MATH/READING/SCIENCE/GENERAL/ADMIN) next to that student's row.
      Picking one hits GET /set-subject, which validates the code/subject/
      that the session is actually open, updates the live session, and
      logs a separate `subject-selected` event — the original check-in
      row is never rewritten, the subject choice is an additional event,
      same "append, don't overwrite" pattern as the correction workflow
      in docs/PILOT_PROTOCOL.md.
    - PRIVACY NOTE, read before using this for real: student check-in
      uses the same anonymous-code discipline as tutors
      (`STUDENT-001`, never a name) but it is a materially bigger privacy
      step than tutor-only tracking — if this is ever used with real
      students, docs/PRIVACY.md and the pilot approval doc's "are minors
      included" question need an honest real answer, not a copy-pasted
      "no." See docs/LAI_MODE.md.

  What changed in v2.0.0 — configurable multi-organization support:
    - The org-specific string ("SPS") is no longer scattered through the
      code. One physical device now picks a single OrganizationConfig
      (see config/organization.example.h) that drives the dashboard
      title, role label, whether a site code is required, and the
      short/long-session thresholds. Same firmware, two real contexts:
      SPS workforce attendance and LAI tutor attendance / volunteer-hour
      verification.
    - The CSV/log schema grew from 8 fields to 14 (event_id, organization,
      site_code, program_code, card_alias, session_id, device_id,
      firmware_version added) so a single exported file is self-describing
      even if records from multiple devices/orgs ever get merged.
    - Raw RFID UIDs are never logged anywhere — only a per-card `alias`
      (e.g. "CARD-A") from the private user registry. This was true in
      practice for the public SPS docs after the v1.4 anonymization pass;
      it's now true in the firmware itself.
    - Added an `active` flag per registered user. An inactive/revoked
      card is denied and logged (status inactive_user) but its history is
      never deleted.
    - Added flagged_long_session: a tutor who's been checked in longer
      than longSessionWarningMs shows as a possible forgotten checkout on
      the dashboard while still checked in, instead of silently staying
      "Active" forever with no signal to the coordinator.
    - Added a session_id (S-YYYYMMDD-NNN) shared by a check-in/check-out
      pair, and a /hours.csv endpoint that sums verified minutes per user
      from whatever event history is actually available (SD file if
      present, else the in-memory ring buffer only — see the honesty note
      in handleHoursExport() below).
    - User/organization data moved out of this file into
      config/users_private.h and config/organization.h (gitignored). The
      committed .example.h versions use fully synthetic data.

  What's explicitly NOT implemented in this firmware, on purpose (see
  docs/LAI_MODE.md for why): forced_checkout, wrong_site, and
  missing_checkout are documented status values used by the *manual
  correction workflow* (an administrator fixing an exported record after
  the fact), not statuses this single-device firmware emits live. Don't
  add code paths for them here without also adding the audit trail
  (original_event / corrected_value / reason / corrected_by /
  correction_timestamp) that makes a correction trustworthy — a
  half-implemented auto-correction is worse than an honest manual one.

  v1.x history (duplicate suppression, short-session flag, SD/LCD
  fail-safe, dashboard) is documented in docs/DEVELOPMENT_LOG.md.

  Libraries required (Arduino Library Manager):
    - MFRC522 by GithubCommunity
    - LiquidCrystal I2C (by Frank de Brabander, or "LiquidCrystal_I2C")
    - SD (bundled with ESP32 board package)
    - WebServer (bundled with ESP32 board package — no install needed)

  Wiring (ESP32 devkit) — unchanged from v1.x, see README.md for the
  Mermaid diagram and full pin tables.

  SECURITY NOTE: Do not commit real WiFi credentials, config/organization.h,
  or config/users_private.h to a public repo. See README.md and
  docs/PRIVACY.md.
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SD.h>

// ---------- Private / per-device configuration ----------
// These two headers are gitignored. If they don't exist yet, copy the
// matching .example.h file from config/ and fill it in locally:
//   config/organization.example.h -> config/organization.h
//   config/users.example.h        -> config/users_private.h
#include "config/organization.h"
#include "config/users_private.h"

const char* FIRMWARE_VERSION = "v2.1.0-student-subject";
const OrganizationConfig &cfg = (ACTIVE_MODE == LAI_MODE) ? LAI_CONFIG : SPS_CONFIG;

// ---------- Pin configuration ----------
#define RFID_SS_PIN   5
#define RFID_RST_PIN  4
#define SD_CS_PIN     15

// LCD I2C address is usually 0x27 or 0x3F. If the screen shows nothing or
// garbage boxes after wiring is confirmed correct, run an I2C scanner
// sketch once (search "Arduino I2C scanner") to find the real address
// and update it here.
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// ---------- Duplicate detection (org-independent) ----------
const unsigned long DUPLICATE_WINDOW_MS = 2000;   // ignore same-user repeat taps within 2s

// ---------- WiFi / NTP (optional — leave blank to skip) ----------
// Fill these in yourself directly in the editor — type them in, don't
// commit real values to git. See README.md for the secrets.h pattern.
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";
const long GMT_OFFSET_SEC = 19800; // IST = UTC+5:30
const int  DAYLIGHT_OFFSET_SEC = 0;
bool wifiTimeAvailable = false;
bool wifiConnected = false;

// ---------- SD card state ----------
bool sdAvailable = false;
const char* LOG_FILE = "/events.csv";

// ---------- LCD state ----------
bool lcdAvailable = false;

// ---------- Web dashboard state ----------
WebServer server(80);
const int MAX_RECENT = 15;
String recentEvents[MAX_RECENT];
int recentCount = 0;
int recentHead = 0;
unsigned long totalEventsLogged = 0;
unsigned long completedSessionsCount = 0;   // since boot — see dashboard note
unsigned long verifiedMinutesSinceBoot = 0; // since boot — see dashboard note
unsigned long nextEventId = 1;

const String CSV_HEADER =
  "event_id,timestamp,organization,site_code,program_code,user_code,role,"
  "card_alias,event_type,session_id,duration_minutes,status,device_id,firmware_version";

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ---------- Session tracking ----------
const char* SUBJECT_CODES[] = {"MATH", "READING", "SCIENCE", "GENERAL", "ADMIN"};
const int NUM_SUBJECT_CODES = sizeof(SUBJECT_CODES) / sizeof(SUBJECT_CODES[0]);

struct Session {
  bool checkedIn;
  unsigned long checkInMillis;
  unsigned long lastScanMillis;
  String sessionId;
  String checkInDisplay;
  // Student sessions only: "" until check-in, then "PENDING" until the
  // student picks a subject on the dashboard, then one of SUBJECT_CODES.
  // Tutor/coordinator sessions never use this — they fall back to the
  // device-wide PROGRAM_CODE.
  String subjectCode;
};
Session sessions[10]; // must be >= NUM_KNOWN_USERS (from config/users_private.h)

// ---------- Session-id generation ----------
String lastSessionDate = "";
int dailySessionCounter = 0;

// ================= Setup =================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("=== " + String(cfg.dashboardTitle) + " (" + String(cfg.organizationCode) + " mode) ===");
  Serial.println("Firmware: " + String(FIRMWARE_VERSION) + "  Device: " + String(DEVICE_ID));

  SPI.begin(); // shared bus for RC522 + SD
  rfid.PCD_Init();

  Wire.begin(21, 22);
  Wire.beginTransmission(LCD_ADDR);
  if (Wire.endTransmission() == 0) {
    lcdAvailable = true;
    lcd.init();
    lcd.backlight();
    Serial.println("LCD initialized at 0x" + String(LCD_ADDR, HEX));
  } else {
    Serial.println("LCD not found on I2C bus — running without display (this is fine, not fatal).");
  }

  initSD();
  // SD.begin() can leave the shared SPI bus in a different state; re-init
  // the RC522 afterward so it isn't left unresponsive on the shared bus.
  rfid.PCD_Init();
  connectWiFiAndSyncTime();

  Serial.println(CSV_HEADER);
  if (sdAvailable && !SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (f) { f.println(CSV_HEADER); f.close(); }
  }

  showIdleScreen();
}

// ================= Main loop =================
void loop() {
  if (wifiConnected) {
    server.handleClient();
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  int userIndex = findUserIndex(rfid.uid.uidByte, rfid.uid.size);

  if (userIndex == -1) {
    logEvent("scan", "denied_unknown_card", -1, -1, "", "");
    showMessage("Unknown Card", "Denied");
  } else if (!knownUsers[userIndex].active) {
    logEvent("scan", "inactive_user", userIndex, -1, "", "");
    showMessage("Card Revoked", knownUsers[userIndex].cardAlias);
  } else if (isDuplicateScan(userIndex)) {
    logEvent("scan", "duplicate_ignored", userIndex, -1, sessions[userIndex].sessionId, sessions[userIndex].subjectCode);
    showMessage("Duplicate Tap", "Ignored");
  } else {
    handleKnownScan(userIndex);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1200); // debounce repeated reads of the same tap
  showIdleScreen();
}

// ================= Core logic =================
bool isDuplicateScan(int userIndex) {
  Session &s = sessions[userIndex];
  if (s.lastScanMillis == 0) return false;
  return (millis() - s.lastScanMillis) < DUPLICATE_WINDOW_MS;
}

void handleKnownScan(int userIndex) {
  User user = knownUsers[userIndex];
  Session &s = sessions[userIndex];
  s.lastScanMillis = millis();

  bool isStudent = (String(user.role) == "student");

  if (!s.checkedIn) {
    s.checkedIn = true;
    s.checkInMillis = millis();
    s.checkInDisplay = getTimestamp();
    s.sessionId = generateSessionId();
    s.subjectCode = isStudent ? "PENDING" : "";
    logEvent("check-in", "success", userIndex, -1, s.sessionId, s.subjectCode);
    showMessage("Checked In", user.cardAlias);
  } else {
    unsigned long elapsedMs = millis() - s.checkInMillis;
    unsigned long durationMin = elapsedMs / 60000UL;
    s.checkedIn = false;

    String status;
    if (elapsedMs < cfg.minValidSessionMs) {
      status = "flagged_short_session";
    } else {
      status = "success";
    }
    // Flagged sessions are still counted, not discarded — they still
    // count toward hours pending human review, per docs/LAI_MODE.md:
    // flags request review, they don't silently erase volunteer hours.
    completedSessionsCount++;
    verifiedMinutesSinceBoot += durationMin;

    String subjectForLog = isStudent ? s.subjectCode : "";
    logEvent("check-out", status, userIndex, (long)durationMin, s.sessionId, subjectForLog);
    showMessage("Checked Out", user.cardAlias + String(" ") + durationMin + "m");
    s.subjectCode = "";
  }
}

int findUserIndex(byte *uid, byte uidSize) {
  for (int i = 0; i < NUM_KNOWN_USERS; i++) {
    bool match = true;
    for (byte j = 0; j < uidSize && j < 4; j++) {
      if (knownUsers[i].uid[j] != uid[j]) { match = false; break; }
    }
    if (match) return i;
  }
  return -1;
}

String generateSessionId() {
  String dateStr = wifiTimeAvailable ? getDateOnly() : "00000000";
  if (dateStr != lastSessionDate) {
    lastSessionDate = dateStr;
    dailySessionCounter = 0;
  }
  dailySessionCounter++;
  char buf[24];
  snprintf(buf, sizeof(buf), "S-%s-%03d", dateStr.c_str(), dailySessionCounter);
  return String(buf);
}

// ================= Logging (Serial always, SD if available, ring buffer always) =================
// Builds the full 14-field CSV row in one place, so every call site shares
// the exact same schema instead of hand-concatenating strings per call
// (the old per-call-site string-building was the root cause of the v1.4
// stray-edit typos documented in docs/DEVELOPMENT_LOG.md).
void logEvent(String eventType, String status, int userIndex, long durationMin, String sessionId, String programCodeOverride) {
  String userCode  = (userIndex >= 0) ? knownUsers[userIndex].userCode  : "-";
  String role      = (userIndex >= 0) ? knownUsers[userIndex].role     : "-";
  String cardAlias = (userIndex >= 0) ? knownUsers[userIndex].cardAlias : "-";
  String durationStr = (durationMin >= 0) ? String(durationMin) : "";
  String siteCode = cfg.requireSiteSelection ? String(SITE_CODE) : "-";
  String programCode;
  if (!cfg.requireSiteSelection) {
    programCode = "-";
  } else if (programCodeOverride.length() > 0) {
    programCode = programCodeOverride; // student's chosen subject, or "PENDING"
  } else {
    programCode = String(PROGRAM_CODE); // tutor/coordinator default
  }

  char eventIdBuf[8];
  snprintf(eventIdBuf, sizeof(eventIdBuf), "E%05lu", nextEventId++);

  String csvLine = String(eventIdBuf) + "," + getTimestamp() + "," + String(cfg.organizationCode) + "," +
                    siteCode + "," + programCode + "," + userCode + "," + role + "," + cardAlias + "," +
                    eventType + "," + sessionId + "," + durationStr + "," + status + "," +
                    String(DEVICE_ID) + "," + String(FIRMWARE_VERSION);

  Serial.println(csvLine);
  totalEventsLogged++;

  // Always push into the in-memory ring buffer, regardless of SD/WiFi
  // state, so the dashboard and CSV export have data even with no SD card.
  recentEvents[recentHead] = csvLine;
  recentHead = (recentHead + 1) % MAX_RECENT;
  if (recentCount < MAX_RECENT) recentCount++;

  if (sdAvailable) {
    File f = SD.open(LOG_FILE, FILE_APPEND);
    if (f) {
      f.println(csvLine);
      f.close();
    } else {
      Serial.println("WARNING: SD write failed, switching to Serial-only logging.");
      sdAvailable = false;
    }
  }
}

void initSD() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card not found — running Serial-only (this is fine, not fatal).");
    sdAvailable = false;
    return;
  }
  sdAvailable = true;
  Serial.println("SD card initialized. Logging to " + String(LOG_FILE));
}

// ================= LCD helpers (16 chars/line, 2 lines) =================
// Both helpers no-op immediately if the LCD wasn't detected at boot, mirroring
// the sdAvailable guard on logEvent() — a missing/unplugged LCD never blocks
// scanning, logging, or anything else.
void showIdleScreen() {
  if (!lcdAvailable) return;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(String(cfg.organizationCode) + " - Scan Card");
  lcd.setCursor(0, 1);
  lcd.print(sdAvailable ? "SD:OK " : "SD:-- ");
  lcd.print(wifiTimeAvailable ? "Time:OK" : "Time:rel");
}

// Truncates to fit 16 chars per line — LCD can't scroll or wrap like the
// OLED could, so keep card aliases short (this is why config/users*.h
// aliases above are short like "CARD-T1", not long strings).
void showMessage(String line1, String line2) {
  if (!lcdAvailable) return;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, LCD_COLS));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, LCD_COLS));
}

// ================= Time handling =================
void connectWiFiAndSyncTime() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("No WiFi configured — using relative on-device time. Dashboard will not be available.");
    return;
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("WiFi connected. IP address: " + WiFi.localIP().toString());

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      wifiTimeAvailable = true;
      Serial.println("WiFi + NTP time synced.");
    }

    setupWebServer();
  } else {
    Serial.println("WiFi connect failed — falling back to relative time. Dashboard will not be available.");
  }
}

String getTimestamp() {
  if (wifiTimeAvailable) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char buf[25];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      return String(buf);
    }
  }
  return "T+" + String(millis() / 1000) + "s";
}

String getDateOnly() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[9];
    strftime(buf, sizeof(buf), "%Y%m%d", &timeinfo);
    return String(buf);
  }
  return "00000000";
}

// ================= Web dashboard =================
// Everything below only ever runs if wifiConnected is true. If WiFi never
// connects, setupWebServer() is never called, server.handleClient() is
// never called in loop(), and the device behaves exactly as v1.x did.
void setupWebServer() {
  server.on("/", handleDashboard);
  server.on("/events.csv", handleCsvExport);
  server.on("/hours.csv", handleHoursExport);
  server.on("/set-subject", handleSetSubject);
  server.begin();
  Serial.println("Dashboard running at http://" + WiFi.localIP().toString() + "/");
}

// A student taps in, then picks their own subject on the dashboard —
// one click, no typing. GET is intentional here (this is a same-network,
// unauthenticated demo/pilot tool, not a form that needs CSRF protection;
// see docs/PRIVACY.md for what "unauthenticated" means before real use).
void handleSetSubject() {
  String code = server.hasArg("code") ? server.arg("code") : "";
  String subject = server.hasArg("subject") ? server.arg("subject") : "";

  int idx = -1;
  for (int i = 0; i < NUM_KNOWN_USERS; i++) {
    if (String(knownUsers[i].userCode) == code) { idx = i; break; }
  }

  bool validSubject = false;
  for (int i = 0; i < NUM_SUBJECT_CODES; i++) {
    if (subject == SUBJECT_CODES[i]) { validSubject = true; break; }
  }

  bool isStudent = (idx >= 0) && (String(knownUsers[idx].role) == "student");

  if (idx == -1 || !validSubject || !isStudent || !sessions[idx].checkedIn) {
    server.send(400, "text/plain", "Invalid or expired subject-selection request.");
    return;
  }

  sessions[idx].subjectCode = subject;
  logEvent("subject-selected", "success", idx, -1, sessions[idx].sessionId, subject);

  server.sendHeader("Location", "/");
  server.send(303);
}

void splitCsv(const String &line, String out[], int maxParts) {
  int start = 0;
  int part = 0;
  for (int i = 0; i <= (int)line.length() && part < maxParts; i++) {
    if (i == (int)line.length() || line[i] == ',') {
      out[part] = line.substring(start, i);
      start = i + 1;
      part++;
    }
  }
}

String statusRowClass(const String &status) {
  if (status == "denied_unknown_card" || status == "inactive_user") return "bad";
  if (status.startsWith("flagged") || status == "duplicate_ignored") return "warn";
  if (status == "success") return "ok";
  return "";
}

void handleDashboard() {
  int checkedInCount = 0;
  for (int i = 0; i < NUM_KNOWN_USERS; i++) {
    if (sessions[i].checkedIn) checkedInCount++;
  }

  String html = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='5'>";
  html += "<title>" + String(cfg.dashboardTitle) + "</title>";
  html += "<style>";
  html += "body{background:#111;color:#eee;font-family:Arial,sans-serif;padding:20px;}";
  html += "h1{font-size:20px;margin-bottom:2px;} h2{font-size:15px;color:#aaa;font-weight:normal;margin-top:0;}";
  html += ".stats{display:flex;gap:10px;margin-bottom:16px;flex-wrap:wrap;}";
  html += ".pill{background:#222;border:1px solid #444;border-radius:8px;padding:6px 12px;font-size:13px;}";
  html += ".ok{color:#4caf50;} .bad{color:#f44336;} .warn{color:#ffb300;}";
  html += "table{border-collapse:collapse;width:100%;font-size:13px;margin-bottom:20px;}";
  html += "th,td{border:1px solid #333;padding:6px 8px;text-align:left;}";
  html += "th{background:#1a1a1a;}";
  html += "a{color:#4caf50;}";
  html += "</style></head><body>";

  html += "<h1>" + String(cfg.dashboardTitle) + "</h1>";
  if (strlen(cfg.dashboardSubtitle) > 0) html += "<h2>" + String(cfg.dashboardSubtitle) + "</h2>";
  if (cfg.requireSiteSelection) html += "<h2>" + String(SITE_CODE) + " &middot; " + String(PROGRAM_CODE) + "</h2>";

  html += "<div class='stats'>";
  html += "<div class='pill'>" + String(cfg.primaryUserLabel) + "s Checked In: <b>" + String(checkedInCount) + "</b></div>";
  html += "<div class='pill'>Completed Sessions (since boot): <b>" + String(completedSessionsCount) + "</b></div>";
  html += "<div class='pill'>Verified Minutes (since boot): <b>" + String(verifiedMinutesSinceBoot) + "</b></div>";
  html += "<div class='pill'>Total Events: <b>" + String(totalEventsLogged) + "</b></div>";
  html += "<div class='pill'>Time: <span class='" + String(wifiTimeAvailable ? "ok" : "bad") + "'>" + String(wifiTimeAvailable ? "NTP" : "relative") + "</span></div>";
  html += "<div class='pill'>Uptime: " + String(millis() / 1000) + "s</div>";
  html += "</div>";

  html += "<p><a href='/events.csv'>Download events CSV</a> &middot; <a href='/hours.csv'>Download hours summary</a></p>";

  // ---- Currently checked-in ----
  html += "<table><tr><th>Who</th><th>Role</th><th>Check-in</th><th>Elapsed</th><th>Subject</th><th>Status</th></tr>";
  bool anyActive = false;
  for (int i = 0; i < NUM_KNOWN_USERS; i++) {
    if (!sessions[i].checkedIn) continue;
    anyActive = true;
    unsigned long elapsedMs = millis() - sessions[i].checkInMillis;
    unsigned long elapsedMin = elapsedMs / 60000UL;
    bool possibleForgotten = (cfg.longSessionWarningMs > 0 && elapsedMs > cfg.longSessionWarningMs);
    bool isStudent = (String(knownUsers[i].role) == "student");
    String code = String(knownUsers[i].userCode);

    String subjectCell = "-";
    if (isStudent) {
      if (sessions[i].subjectCode == "" || sessions[i].subjectCode == "PENDING") {
        subjectCell = "";
        for (int s = 0; s < NUM_SUBJECT_CODES; s++) {
          if (s > 0) subjectCell += " &middot; ";
          subjectCell += "<a href='/set-subject?code=" + code + "&subject=" + String(SUBJECT_CODES[s]) + "'>" + String(SUBJECT_CODES[s]) + "</a>";
        }
      } else {
        subjectCell = sessions[i].subjectCode;
      }
    }

    html += "<tr>";
    html += "<td>" + String(knownUsers[i].cardAlias) + "</td>";
    html += "<td>" + String(knownUsers[i].role) + "</td>";
    html += "<td>" + sessions[i].checkInDisplay + "</td>";
    html += "<td>" + String(elapsedMin) + " min</td>";
    html += "<td>" + subjectCell + "</td>";
    html += possibleForgotten
      ? "<td class='warn'>Active — possible forgotten checkout</td>"
      : "<td class='ok'>Active</td>";
    html += "</tr>";
  }
  if (!anyActive) html += "<tr><td colspan='6'>No one currently checked in.</td></tr>";
  html += "</table>";

  // ---- Recent events ----
  html += "<table><tr><th>Time</th><th>User</th><th>Card</th><th>Event</th><th>Subject</th><th>Session</th><th>Duration</th><th>Status</th></tr>";
  for (int i = 0; i < recentCount; i++) {
    int idx = (recentHead - 1 - i + MAX_RECENT) % MAX_RECENT;
    String f[14];
    splitCsv(recentEvents[idx], f, 14);
    // fields: 0 event_id,1 timestamp,2 organization,3 site_code,4 program_code,
    // 5 user_code,6 role,7 card_alias,8 event_type,9 session_id,
    // 10 duration_minutes,11 status,12 device_id,13 firmware_version
    String rowClass = statusRowClass(f[11]);
    html += "<tr>";
    html += "<td>" + f[1] + "</td>";
    html += "<td>" + f[5] + "</td>";
    html += "<td>" + f[7] + "</td>";
    html += "<td>" + f[8] + "</td>";
    html += "<td>" + f[4] + "</td>";
    html += "<td>" + f[9] + "</td>";
    html += "<td>" + f[10] + "</td>";
    html += "<td class='" + rowClass + "'>" + f[11] + "</td>";
    html += "</tr>";
  }
  if (recentCount == 0) {
    html += "<tr><td colspan='8'>No scans yet.</td></tr>";
  }
  html += "</table></body></html>";
  server.send(200, "text/html", html);
}

void handleCsvExport() {
  if (sdAvailable && SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_READ);
    if (f) {
      server.sendHeader("Content-Disposition", "attachment; filename=attendance.csv");
      server.streamFile(f, "text/csv");
      f.close();
      return;
    }
  }

  // Fall back to the in-memory ring buffer if there's no SD card — the
  // export still works, it just only covers events since the last boot
  // instead of the full history.
  String csv = CSV_HEADER + "\n";
  for (int i = 0; i < recentCount; i++) {
    int idx = (recentHead - 1 - i + MAX_RECENT) % MAX_RECENT;
    csv += recentEvents[idx] + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=attendance.csv");
  server.send(200, "text/csv", csv);
}

// Sums verified minutes per registered user from whatever event history is
// actually available right now.
//
// HONESTY NOTE: this is NOT a date-range query and must never be described
// as one. If an SD card is present, it sums the *entire* SD CSV file since
// it was created (could be days). If there's no SD card, it only has the
// last 15 events since the last reboot (the ring buffer). Which one you get
// depends entirely on hardware state, and the page doesn't currently tell
// you which mode it used — that's a real gap, not a rounding error; fix it
// before treating this as an official hours report for anyone outside a
// demo/pilot.
void handleHoursExport() {
  int minutesByUser[10];
  int flaggedByUser[10];
  for (int i = 0; i < NUM_KNOWN_USERS; i++) { minutesByUser[i] = 0; flaggedByUser[i] = 0; }

  auto accumulateLine = [&](const String &line) {
    if (line.length() == 0 || line.startsWith("event_id")) return;
    String f[14];
    splitCsv(line, f, 14);
    if (f[8] != "check-out") return; // only check-out rows carry duration
    for (int i = 0; i < NUM_KNOWN_USERS; i++) {
      if (f[5] == String(knownUsers[i].userCode)) {
        if (f[11] == "success" || f[11].startsWith("flagged")) {
          minutesByUser[i] += f[10].toInt();
          if (f[11].startsWith("flagged")) flaggedByUser[i]++;
        }
      }
    }
  };

  if (sdAvailable && SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_READ);
    if (f) {
      while (f.available()) {
        accumulateLine(f.readStringUntil('\n'));
      }
      f.close();
    }
  } else {
    for (int i = 0; i < recentCount; i++) {
      int idx = (recentHead - 1 - i + MAX_RECENT) % MAX_RECENT;
      accumulateLine(recentEvents[idx]);
    }
  }

  String csv = "user_code,role,card_alias,verified_minutes,flagged_sessions_included,source\n";
  String source = (sdAvailable && SD.exists(LOG_FILE)) ? "sd_full_history" : "ring_buffer_since_boot_only";
  for (int i = 0; i < NUM_KNOWN_USERS; i++) {
    csv += String(knownUsers[i].userCode) + "," + String(knownUsers[i].role) + "," +
           String(knownUsers[i].cardAlias) + "," + String(minutesByUser[i]) + "," +
           String(flaggedByUser[i]) + "," + source + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=hours_summary.csv");
  server.send(200, "text/csv", csv);
}
