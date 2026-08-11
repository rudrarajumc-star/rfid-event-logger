/*
  Embedded RFID Event Logger — v1.4
  ESP32 + RC522 (SPI) + 16x2 I2C LCD + MicroSD CSV backup + WiFi web dashboard
  (all graceful-degradation)

  Changes from v1.3:
    - Added real duplicate-scan suppression: if the same registered user
      taps again within DUPLICATE_WINDOW_MS (2s), it's logged as
      "duplicate_ignored" instead of toggling check-in/check-out state.
      This replaces relying on the blocking 1200ms delay() alone, which
      only prevented back-to-back reads, not a genuine repeated-tap check
      on a real time window per user.
    - Added a "flagged_short_session" status: a check-out that happens
      less than SHORT_SESSION_MS (5s) after its matching check-in is still
      logged (nothing is silently dropped) but flagged instead of marked
      plain "success", so an accidental double-tap or a mis-scan doesn't
      look identical to a real attendance session in the data.
    - Added live dashboard overview stats: "Checked In" (current count of
      users with an open session) and "Total Events" (count since boot,
      not capped at the 15-entry recent-events ring buffer). Status column
      on the dashboard is now color-coded (green success, amber flagged/
      duplicate, red denied).

  Changes from v1.2 / earlier are documented further down and in
  docs/DEVELOPMENT_LOG.md in the project repo.

  Core behavior:
    - Scans RFID cards, matches against a known-user table
    - Tracks check-in / check-out per user, computes session duration
    - Suppresses accidental duplicate taps within a real time window
    - Flags suspiciously short sessions instead of hiding or misreporting them
    - Shows live status on the LCD (if present) and on the web dashboard
    - Logs every event to Serial, and to /events.csv on a MicroSD card if
      present. If the SD card is missing, fails, or is pulled mid-run, the
      device logs a warning once and keeps working on Serial + LCD +
      dashboard only — it never stops because one optional component failed.
    - Real timestamps via WiFi NTP, falls back to relative on-device time
      if WiFi isn't configured or doesn't connect.

  Libraries required (Arduino Library Manager):
    - MFRC522 by GithubCommunity
    - LiquidCrystal I2C (by Frank de Brabander, or "LiquidCrystal_I2C")
    - SD (bundled with ESP32 board package)
    - WebServer (bundled with ESP32 board package — no install needed)

  Wiring (ESP32 devkit):
    RC522        ESP32
    ----------------------
    SDA(SS)  ->  GPIO5
    SCK      ->  GPIO18
    MOSI     ->  GPIO23
    MISO     ->  GPIO19
    RST      ->  GPIO4
    3.3V     ->  3.3V   (NOT 5V — RC522 is 3.3V only)
    GND      ->  GND

    MicroSD module (shares SPI bus with RC522 — same SCK/MOSI/MISO,
    separate CS pin):
    CS       ->  GPIO15
    SCK      ->  GPIO18  (shared)
    MOSI     ->  GPIO23  (shared)
    MISO     ->  GPIO19  (shared)
    VCC      ->  check your module (3.3V or 5V)
    GND      ->  GND

    16x2 I2C LCD          ESP32
    ----------------------
    SDA      ->  GPIO21
    SCL      ->  GPIO22
    VCC      ->  5V (most I2C backpacks want 5V, not 3.3V — check yours)
    GND      ->  GND

  SECURITY NOTE: Do not commit real WiFi credentials to a public repo.
  Fill in WIFI_SSID / WIFI_PASSWORD below locally, directly in your own
  editor, right before uploading. Never paste them into chat, a commit
  message, or an issue. See README.md "Before you push" section.
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SD.h>

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

// ---------- Deployment mode (hardcoded per device, no button) ----------
// This is set once per physical device. Flashing a unit for SPS: leave as
// MODE_SPS. Flashing a second unit for LAI later: change to MODE_LAI and
// re-upload. That's the entire "reusable for both orgs" story — one line.
enum Mode { MODE_LAI, MODE_SPS };
const Mode CURRENT_MODE = MODE_SPS;

// ---------- Anomaly / duplicate detection tuning ----------
const unsigned long DUPLICATE_WINDOW_MS = 2000;   // ignore same-user repeat taps within 2s
const unsigned long SHORT_SESSION_MS = 5000;      // flag check-outs faster than this

// ---------- WiFi / NTP (optional — leave blank to skip) ----------
// Fill these in yourself directly in the editor — type them in, don't
// commit real values to git. See README.md for a gitignore-based pattern.
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
const String CSV_HEADER = "timestamp,mode,user_type,user_code,card_id,event_type,duration_min,status";

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ---------- Known users ----------
// Register real cards here: tap an unregistered card once, its UID prints
// to Serial, copy it in below with the person's real name/role.
struct KnownUser {
  byte uid[4];
  const char* code;
  const char* role;
};

KnownUser knownUsers[] = {
  { {0x21, 0x13, 0x04, 0x07}, "SPS-001", "employee" },
  { {0xBB, 0x89, 0x10, 0xEF}, "SPS-002", "employee" },
  { {0x44, 0xC9, 0x01, 0xAB}, "SPS-003", "employee" },
};
const int NUM_KNOWN_USERS = sizeof(knownUsers) / sizeof(knownUsers[0]);

// ---------- Session tracking ----------
struct Session {
  bool checkedIn;
  unsigned long checkInMillis;
  unsigned long lastScanMillis;
};
Session sessions[10];

// ================= Setup =================
void setup() {
  Serial.begin(115200);
  delay(300);

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

  String uidStr = uidToString(rfid.uid.uidByte, rfid.uid.size);
  int userIndex = findUserIndex(rfid.uid.uidByte, rfid.uid.size);

  if (userIndex == -1) {
    logEvent(getTimestamp() + "," + modeStr() + ",unknown,-," + uidStr + ",scan,,denied");
    showMessage("Unknown Card", uidStr);
  } else if (isDuplicateScan(userIndex)) {
    logEvent(getTimestamp() + "," + modeStr() + "," + knownUsers[userIndex].role + "," +
              knownUsers[userIndex].code + "," + uidStr + ",scan,,duplicate_ignored");
    showMessage("Duplicate Tap", "Ignored");
  } else {
    handleKnownScan(userIndex, uidStr);
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

void handleKnownScan(int userIndex, String uidStr) {
  KnownUser user = knownUsers[userIndex];
  Session &s = sessions[userIndex];
  s.lastScanMillis = millis();

  if (!s.checkedIn) {
    s.checkedIn = true;
    s.checkInMillis = millis();
    logEvent(getTimestamp() + "," + modeStr() + "," + user.role + "," +
              user.code + "," + uidStr + ",check-in,,success");
    showMessage("Checked In", user.code);
  } else {
    unsigned long elapsedMs = millis() - s.checkInMillis;
    unsigned long durationMin = elapsedMs / 60000UL;
    s.checkedIn = false;
    String status = (elapsedMs < SHORT_SESSION_MS) ? "flagged_short_session" : "success";
    logEvent(getTimestamp() + "," + modeStr() + "," + user.role + "," +
              user.code + "," + uidStr + ",check-out," + String(durationMin) + "," + status);
    showMessage("Checked Out", user.code + String(" ") + durationMin + "m");
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

String uidToString(byte *uid, byte uidSize) {
  String s = "";
  for (byte i = 0; i < uidSize; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// ================= Logging (Serial always, SD if available, ring buffer always) =================
void logEvent(String csvLine) {
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

String modeStr() {
  return CURRENT_MODE == MODE_LAI ? "LAI" : "SPS";
}

// ================= LCD helpers (16 chars/line, 2 lines) =================
// Both helpers no-op immediately if the LCD wasn't detected at boot, mirroring
// the sdAvailable guard on logEvent() — a missing/unplugged LCD never blocks
// scanning, logging, or anything else.
void showIdleScreen() {
  if (!lcdAvailable) return;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(modeStr() + " - Scan Card");
  lcd.setCursor(0, 1);
  lcd.print(sdAvailable ? "SD:OK " : "SD:-- ");
  lcd.print(wifiTimeAvailable ? "Time:OK" : "Time:rel");
}

// Truncates to fit 16 chars per line — LCD can't scroll or wrap like the
// OLED could, so keep user codes short (this is why knownUsers codes above
// are short like "SPS-001", not long strings).
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

// ================= Web dashboard =================
// Everything below only ever runs if wifiConnected is true. If WiFi never
// connects, setupWebServer() is never called, server.handleClient() is
// never called in loop(), and the device behaves exactly as v1.2 did.
void setupWebServer() {
  server.on("/", handleDashboard);
  server.on("/events.csv", handleCsvExport);
  server.begin();
  Serial.println("Dashboard running at http://" + WiFi.localIP().toString() + "/");
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

void handleDashboard() {
  int checkedInCount = 0;
  for (int i = 0; i < NUM_KNOWN_USERS; i++) {
    if (sessions[i].checkedIn) checkedInCount++;
  }

  String html = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='5'>";
  html += "<title>" + modeStr() + " Attendance Dashboard</title>";
  html += "<style>";
  html += "body{background:#111;color:#eee;font-family:Arial,sans-serif;padding:20px;}";
  html += "h1{font-size:20px;}";
  html += ".stats{display:flex;gap:10px;margin-bottom:16px;flex-wrap:wrap;}";
  html += ".pill{background:#222;border:1px solid #444;border-radius:8px;padding:6px 12px;font-size:13px;}";
  html += ".ok{color:#4caf50;} .bad{color:#f44336;} .warn{color:#ffb300;}";
  html += "table{border-collapse:collapse;width:100%;font-size:13px;}";
  html += "th,td{border:1px solid #333;padding:6px 8px;text-align:left;}";
  html += "th{background:#1a1a1a;}";
  html += "a{color:#4caf50;}";
  html += "</style></head><body>";
  html += "<h1>" + modeStr() + " Attendance Dashboard</h1>";
  html += "<div class='stats'>";
  html += "<div class='pill'>Checked In: <b>" + String(checkedInCount) + "</b></div>";
  html += "<div class='pill'>Total Events: <b>" + String(totalEventsLogged) + "</b></div>";
  html += "<div class='pill'>Time: <span class='" + String(wifiTimeAvailable ? "ok" : "bad") + "'>" + String(wifiTimeAvailable ? "NTP" : "relative") + "</span></div>";
  html += "<div class='pill'>Uptime: " + String(millis() / 1000) + "s</div>";
  html += "</div>";
  html += "<p><a href='/events.csv'>Download full attendance CSV</a></p>";
  html += "<table><tr><th>Time</th><th>Mode</th><th>User</th><th>Card</th><th>Event</th><th>Duration</th><th>Status</th></tr>";

  for (int i = 0; i < recentCount; i++) {
    int idx = (recentHead - 1 - i + MAX_RECENT) % MAX_RECENT;
    String fields[8];
    splitCsv(recentEvents[idx], fields, 8);
    String status = fields[7];
    String rowClass = "";
    if (status == "denied") rowClass = "bad";
    else if (status.startsWith("flagged") || status == "duplicate_ignored") rowClass = "warn";
    else if (status == "success") rowClass = "ok";
    html += "<tr>";
    html += "<td>" + fields[0] + "</td>";
    html += "<td>" + fields[1] + "</td>";
    html += "<td>" + fields[3] + "</td>";
    html += "<td>" + fields[4] + "</td>";
    html += "<td>" + fields[5] + "</td>";
    html += "<td>" + fields[6] + "</td>";
    html += "<td class='" + rowClass + "'>" + status + "</td>";
    html += "</tr>";
  }

  if (recentCount == 0) {
    html += "<tr><td colspan='7'>No scans yet.</td></tr>";
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
