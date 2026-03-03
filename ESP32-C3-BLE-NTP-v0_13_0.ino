// ESP32-C3 BLE NTP v0.13.0
// Step 2 -- BLE bonding (saves trusted device to flash)
//   - Auth mode ESP_LE_AUTH_REQ_SC_MITM_BOND -- enables bond storage in ESP32 stack
//   - Bond helpers: saveBond(), updateBondTimestamp(), isBonded(), getBondCount()
//   - Bond stored in Preferences namespace "bonds" -- up to MAX_BONDS (5) devices
//   - Each bond: "bondN" key, value "MAC:timestamp" -- oldest evicted when full
//   - onAuthenticationComplete saves bond to Preferences on pairing success
//   - onConnect logs bond count and clears currentPeerMAC for fresh capture
//   - SecurityCallbacks and BLESecurity declared static in startBLE()
//     new BLESecurity() after deinit() corrupts heap -- static prevents reallocation
//   - BLEDevice::init() called once only -- bleInitialized flag prevents reinit
//   - deinit(true) replaced with stopAdvertising() everywhere -- no heap corruption
//   - pServer / pCharTX stay valid between BLE sessions -- no null on stop
//   - Verbose serial logging on all bond operations
// Step 1 -- BLE passkey pairing
//   - Numeric Comparison mode -- ESP_IO_CAP_IO
//   - Stack generates same 6-digit code on both ESP32 and phone
//   - onConfirmPIN() displays code on matrix, auto-confirms our side, returns true
//   - Phone shows same number -- user taps confirm to complete pairing
//   - showPIN flag set in callback -- loop() calls ShowString() to avoid blocking stack
//   - ShowChar() / ShowString() added -- instant display, 400ms per digit, no fade
//   - COL_WHITE for PIN display -- distinct from time (amber) and date (blue)
//   - Verbose serial logging on every security event for debugging
// Carried forward from v0.12.6:
//   - time? returns time only, respects hour12 -- safe under 20 byte BLE limit
//   - date? new command -- returns date and timezone string
//   - "rbt" matrix message restored in pass:, tz:, reboot: before ESP.restart()
//   - pass: reboots after saving credentials -- syncNTP() cannot run while BLE active (shared radio)
//   - tz: always saves and reboots -- myTZ.setLocation() needs WiFi, cannot run while BLE active
// Carried forward from v0.12.5:
//   - chaseAnimation(colour), radarAnimation(), WiFi retry loop, auto BLE on failure
// Carried forward from v0.12.4:
//   - info? BLE command, resync:N, periodic NTP re-sync
//   - bright:N, clock12:, clock24:, flip:on/off BLE commands
//   - bleActive flag comments on all deinit() paths
//   - PROPERTY_WRITE_NR required on ESP32-C3 (plain WRITE drops callbacks silently)
//   - Full flash erase required before first upload (Tools -> Erase All Flash)
//   - After NTP sync WiFi and BLE both off -- radios idle until button pressed

#include <Adafruit_NeoPixel.h>
#include <ezTime.h>
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include "font.h"

// ---------------------------------------------------------------------------
// Pin and matrix config
// ---------------------------------------------------------------------------
#define CHAR_WIDTH      5
#define CHAR_HEIGHT     5
#define LED_PIN         8
#define LED_COUNT       25

// ---------------------------------------------------------------------------
// Timing constants
// ---------------------------------------------------------------------------
#define TIME_REFRESH_MS   20000
#define FADE_STEP_MS      30
#define FADE_HOLD_MS      200
#define BTN_PIN           9
#define LONG_PRESS_MS     3000
#define STATUS_LED_PIN    10
#define WIFI_ATTEMPT_MS   10000  // timeout per WiFi attempt (ms)
#define WIFI_MAX_ATTEMPTS 3      // total attempts before entering BLE config
#define CHASE_STEP_MS     50     // ms per chase animation frame
#define RADAR_STEP_MS     200    // ms per radar ring frame

// ---------------------------------------------------------------------------
// BLE UUIDs
// ---------------------------------------------------------------------------
#define SERVICE_UUID "12345678-1234-1234-1234-123456789abc"
#define CHAR_UUID_RX "abcd1234-ab12-ab12-ab12-abcdef012345"
#define CHAR_UUID_TX "abcd1235-ab12-ab12-ab12-abcdef012345"

// ---------------------------------------------------------------------------
// Colour constants
// ---------------------------------------------------------------------------
const uint32_t COL_RED   = 0xFF0000;
const uint32_t COL_GREEN = 0x00FF00;
const uint32_t COL_BLUE  = 0x0000FF;
const uint32_t COL_AMBER = 0x64C800;
const uint32_t COL_WHITE = 0xFFFFFF; // PIN display -- distinct from time (amber) and date (blue)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;
Timezone myTZ;

char tzName[40]   = "America/Edmonton";
char wifiSSID[64] = "";
char wifiPass[64] = "";

BLEServer*         pServer       = nullptr;
BLECharacteristic* pCharTX       = nullptr;
bool               bleConnected  = false;
bool               bleActive      = false; // MUST be reset to false after every stopAdvertising()
bool               bleInitialized = false; // BLEDevice::init() called only once -- never reinit after deinit()
uint32_t           currentPasskey = 0;    // passkey from onConfirmPIN -- stored for reference
bool               showPIN        = false; // set by onConfirmPIN -- loop() reads and displays
char               pinToShow[7]   = "";    // 6-digit PIN string set by onConfirmPIN
unsigned long      btnPressStart  = 0;
bool               ntpSynced      = false;

// Display settings -- loaded from Preferences on boot
uint8_t  brightLevel = 5;    // 1-10, maps to level * 15 (15-150)
bool     hour12      = false; // false = 24h (default), true = 12h
bool     flipped     = false; // false = USB bottom (default), true = USB top

// NTP re-sync settings
uint8_t       resyncHours  = 100;
unsigned long lastSync     = 0;
unsigned long lastSyncUnix = 0;

// BLE auto-timeout -- shuts BLE off if unused
// 3 minutes with no connection, 1 minute after disconnect
#define BLE_ADVERTISE_TIMEOUT_MS  180000
#define BLE_DISCONNECT_TIMEOUT_MS  60000
unsigned long bleStartedAt      = 0;
unsigned long bleDisconnectedAt = 0;

// ---------------------------------------------------------------------------
// wait() -- non-blocking delay with yield()
// ---------------------------------------------------------------------------
void wait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) yield();
}

// ---------------------------------------------------------------------------
// timeFormat() -- returns format string based on current 12/24 setting
// ---------------------------------------------------------------------------
const char* timeFormat() {
  return hour12 ? "g:i A" : "H:i";
}

// ---------------------------------------------------------------------------
// uptimeString() -- returns uptime as "Xd Yh Zm" from millis()
// ---------------------------------------------------------------------------
String uptimeString() {
  unsigned long s = millis() / 1000;
  uint8_t d = s / 86400; s %= 86400;
  uint8_t h = s / 3600;  s %= 3600;
  uint8_t m = s / 60;
  return String(d) + "d " + String(h) + "h " + String(m) + "m";
}

// ---------------------------------------------------------------------------
// chaseAnimation() -- three pixels chasing each other across the matrix
// Called each iteration of WiFi connect loop -- millis() drives position
// Three pixels spaced evenly (LED_COUNT/3 apart) sweep 0-24 continuously
// Colour changes per WiFi attempt: blue=1, amber=2, red=3
// ---------------------------------------------------------------------------
void chaseAnimation(uint32_t colour) {
  uint8_t pos = (millis() / CHASE_STEP_MS) % LED_COUNT;
  strip.clear();
  strip.setPixelColor(pos,                          colour);
  strip.setPixelColor((pos + LED_COUNT/3)  % LED_COUNT, colour);
  strip.setPixelColor((pos + LED_COUNT*2/3) % LED_COUNT, colour);
  strip.show();
}

// ---------------------------------------------------------------------------
// radarAnimation() -- rings expanding from centre of 5x5 matrix
// Called each iteration of BLE advertise loop -- millis() drives ring index
//
// Ring layout on 5x5 matrix (LED index, USB bottom orientation):
//   Ring 0 (centre):  {12}
//   Ring 1 (inner):   {6, 7, 8, 11, 13, 16, 17, 18}
//   Ring 2 (outer):   {0,1,2,3,4, 5,9,10,14,15,19,20,21,22,23,24}
//   Ring 3 (blank):   all off -- pause before repeat
// ---------------------------------------------------------------------------
void radarAnimation() {
  // Ring pixel sets -- raw LED indices, USB bottom orientation
  static const uint8_t ring0[] = {12};
  static const uint8_t ring1[] = {6, 7, 8, 11, 13, 16, 17, 18};
  static const uint8_t ring2[] = {0, 1, 2, 3, 4, 5, 9, 10, 14, 15, 19, 20, 21, 22, 23, 24};

  uint8_t frame = (millis() / RADAR_STEP_MS) % 4; // 0=centre, 1=inner, 2=outer, 3=blank

  strip.clear();
  if (frame == 0) {
    for (uint8_t i = 0; i < sizeof(ring0); i++) strip.setPixelColor(ring0[i], COL_GREEN);
  } else if (frame == 1) {
    for (uint8_t i = 0; i < sizeof(ring1); i++) strip.setPixelColor(ring1[i], COL_GREEN);
  } else if (frame == 2) {
    for (uint8_t i = 0; i < sizeof(ring2); i++) strip.setPixelColor(ring2[i], COL_GREEN);
  }
  // frame 3: all off -- blank pause before repeat
  strip.show();
}

// ---------------------------------------------------------------------------
// Drawing functions
// ---------------------------------------------------------------------------
void DrawPixel(uint32_t colour, uint8_t x, uint8_t y, uint8_t brightness) {
  // flipped = false: USB bottom (default) -- 0,0 is bottom left
  // flipped = true:  USB top (180 degrees) -- 0,0 is top right
  uint8_t led = flipped ? (y * 5 + x) : ((4 - y) * 5 + (4 - x));
  uint8_t r = ((colour >> 16) & 0xff) * brightness / 100;
  uint8_t g = ((colour >>  8) & 0xff) * brightness / 100;
  uint8_t b = ( colour        & 0xff) * brightness / 100;
  strip.setPixelColor(led, r, g, b);
}

void DrawChar(uint32_t colour, char c, uint8_t brightness) {
  c = c & 0x7F;
  if (c < ' ') { c = 0; } else { c -= ' '; }
  const uint8_t* chr = font[(uint8_t)c];
  for (uint8_t x = 0; x < CHAR_WIDTH; x++)
    for (uint8_t y = 0; y < CHAR_HEIGHT; y++)
      if (chr[x] >> 2 & (1 << y))
        DrawPixel(colour, x, y, brightness);
  strip.show();
}

void FadeChar(uint32_t colour, char c) {
  for (uint8_t br = 20; br <= 100; br += 5) { DrawChar(colour, c, br); wait(FADE_STEP_MS); }
  wait(FADE_HOLD_MS);
  for (uint8_t br = 100; br > 0; br -= 5)  { DrawChar(colour, c, br); wait(FADE_STEP_MS); }
  strip.clear();
  strip.show();
}

void FadeString(uint32_t colour, const char* s) {
  for (int i = 0; s[i] != '\0'; i++) FadeChar(colour, s[i]);
}

// ---------------------------------------------------------------------------
// ShowChar() / ShowString() -- instant display, no fade
// Used for PIN display -- each digit shown at full brightness for PIN_HOLD_MS
// Called from loop() after onConfirmPIN sets showPIN flag
// ---------------------------------------------------------------------------
#define PIN_HOLD_MS 400 // ms per PIN digit -- fast enough to show all 6 before phone times out

void ShowChar(uint32_t colour, char c) {
  strip.clear();
  DrawChar(colour, c, 100);  // full brightness, no fade
  wait(PIN_HOLD_MS);
  strip.clear();
  strip.show();
}

void ShowString(uint32_t colour, const char* s) {
  Serial.println("[PIN] Displaying on matrix: " + String(s));
  for (int i = 0; s[i] != '\0'; i++) ShowChar(colour, s[i]);
  Serial.println(F("[PIN] Display complete"));
}

// ---------------------------------------------------------------------------
// bleNotify() -- send string to phone over TX characteristic
// ---------------------------------------------------------------------------
void bleNotify(const char* msg) {
  if (pCharTX && bleConnected) {
    pCharTX->setValue(msg);
    pCharTX->notify();
  }
}

// ---------------------------------------------------------------------------
// bleNotifyLine() -- notify with 50ms pause after
// Used by info? to avoid dropping packets on back-to-back notifies
// ---------------------------------------------------------------------------
void bleNotifyLine(const char* msg) {
  bleNotify(msg);
  wait(50);
}

// ---------------------------------------------------------------------------
// syncNTP() -- connect WiFi with retry loop, sync time, disconnect
// 3 attempts max -- shows attempt number and chase animation per attempt
// On 3 failures: clears display, returns -- caller handles BLE fallback
// ---------------------------------------------------------------------------
void syncNTP() {
  prefs.begin("clock", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String tz   = prefs.getString("tz", "America/Edmonton");
  prefs.end();
  tz.toCharArray(tzName, 40);

  // Store SSID for info? display
  ssid.toCharArray(wifiSSID, 64);

  // Attempt colours: attempt 1=blue, 2=amber, 3=red
  const uint32_t attemptColour[WIFI_MAX_ATTEMPTS] = { COL_BLUE, COL_AMBER, COL_RED };

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm); // reduce TX power to prevent brownout

  for (uint8_t attempt = 0; attempt < WIFI_MAX_ATTEMPTS; attempt++) {
    Serial.println("[+] WiFi attempt " + String(attempt + 1) + " of " + String(WIFI_MAX_ATTEMPTS) + ": " + ssid);

    // Show attempt number in red then animate while connecting
    FadeChar(COL_RED, '1' + attempt);

    digitalWrite(STATUS_LED_PIN, HIGH);
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      chaseAnimation(attemptColour[attempt]); // animate while waiting
      yield();
    }

    strip.clear();
    strip.show();

    if (WiFi.status() == WL_CONNECTED) {
      // Connected -- sync NTP and return
      Serial.println(F("[+] WiFi connected -- syncing NTP"));
      waitForSync();
      Serial.println("[+] UTC: " + UTC.dateTime());

      if (myTZ.setLocation(tzName)) {
        Serial.println("[+] TZ OK: " + String(tzName));
        Serial.println("[+] Local: " + myTZ.dateTime());
        ntpSynced    = true;
        lastSync     = millis();
        lastSyncUnix = UTC.now();
      } else {
        Serial.println(F("[-] TZ failed, using UTC"));
        myTZ         = UTC;
        ntpSynced    = true;
        lastSync     = millis();
        lastSyncUnix = UTC.now();
      }

      WiFi.disconnect();
      WiFi.mode(WIFI_OFF);
      digitalWrite(STATUS_LED_PIN, LOW);
      Serial.println(F("[+] WiFi off -- hold button to start BLE"));
      return; // success -- exit retry loop
    }

    // Attempt failed -- disconnect before next try
    Serial.println("[!] Attempt " + String(attempt + 1) + " failed");
    WiFi.disconnect();
    digitalWrite(STATUS_LED_PIN, LOW);

    if (attempt < WIFI_MAX_ATTEMPTS - 1) {
      wait(2000); // brief pause between attempts
    }
  }

  // All attempts failed -- caller will start BLE
  WiFi.mode(WIFI_OFF);
  Serial.println(F("[-] All WiFi attempts failed -- entering BLE config"));
}

// ---------------------------------------------------------------------------
// Bond storage -- up to MAX_BONDS devices stored in Preferences
// Each bond stored as "bondN" key, value = "MAC:timestamp"
// On new bond: update timestamp if MAC exists, else find empty slot or evict oldest
// ---------------------------------------------------------------------------
#define MAX_BONDS 5

char currentPeerMAC[18] = ""; // set in onConnect -- used by saveBond()

// getBondCount() -- returns number of currently stored bonds
uint8_t getBondCount() {
  prefs.begin("bonds", true);
  uint8_t count = 0;
  char key[7];
  for (uint8_t i = 0; i < MAX_BONDS; i++) {
    snprintf(key, sizeof(key), "bond%d", i);
    if (prefs.getString(key, "").length() > 0) count++;
  }
  prefs.end();
  return count;
}

// saveBond() -- save or update bond for given MAC address
// If MAC already stored -- update timestamp only
// If slot available -- save to empty slot
// If all slots full -- evict oldest last-seen and replace
void saveBond(const char* mac) {
  if (strlen(mac) == 0) {
    Serial.println(F("[BOND] saveBond called with empty MAC -- skipping"));
    return;
  }
  Serial.println("[BOND] Saving bond for: " + String(mac));

  prefs.begin("bonds", false);
  char key[7];
  int  emptySlot   = -1;
  int  matchSlot   = -1;
  int  oldestSlot  = -1;
  unsigned long oldestTime = ULONG_MAX;

  for (uint8_t i = 0; i < MAX_BONDS; i++) {
    snprintf(key, sizeof(key), "bond%d", i);
    String val = prefs.getString(key, "");

    if (val.length() == 0) {
      if (emptySlot < 0) emptySlot = i; // track first empty slot
      continue;
    }

    // Parse "MAC:timestamp" -- last colon separates MAC from timestamp
    // MAC format: "AA:BB:CC:DD:EE:FF" -- lastIndexOf finds the right separator
    int lastColon = val.lastIndexOf(':');
    String storedMAC = val.substring(0, lastColon);
    unsigned long storedTime = val.substring(lastColon + 1).toInt();

    if (storedMAC.equalsIgnoreCase(mac)) {
      matchSlot = i; // MAC already stored -- just update timestamp
      break;
    }
    if (storedTime < oldestTime) {
      oldestTime = storedTime;
      oldestSlot = i; // track oldest for eviction
    }
  }

  String newVal = String(mac) + ":" + String(millis() / 1000);

  if (matchSlot >= 0) {
    // MAC already known -- update timestamp
    snprintf(key, sizeof(key), "bond%d", matchSlot);
    prefs.putString(key, newVal);
    Serial.println("[BOND] Updated timestamp for existing bond in slot " + String(matchSlot));
  } else if (emptySlot >= 0) {
    // Empty slot available -- save there
    snprintf(key, sizeof(key), "bond%d", emptySlot);
    prefs.putString(key, newVal);
    Serial.println("[BOND] Saved new bond in slot " + String(emptySlot));
  } else if (oldestSlot >= 0) {
    // All slots full -- evict oldest
    snprintf(key, sizeof(key), "bond%d", oldestSlot);
    Serial.println("[BOND] All slots full -- evicting oldest bond from slot " + String(oldestSlot));
    prefs.putString(key, newVal);
    Serial.println("[BOND] Saved new bond in slot " + String(oldestSlot));
  }

  prefs.end();
  Serial.println("[BOND] Total bonds stored: " + String(getBondCount()));
}

// updateBondTimestamp() -- refresh last-seen time for a known MAC
void updateBondTimestamp(const char* mac) {
  if (strlen(mac) == 0) return;
  prefs.begin("bonds", false);
  char key[7];
  for (uint8_t i = 0; i < MAX_BONDS; i++) {
    snprintf(key, sizeof(key), "bond%d", i);
    String val = prefs.getString(key, "");
    if (val.length() == 0) continue;
    int lastColon = val.lastIndexOf(':');
    String storedMAC = val.substring(0, lastColon);
    if (storedMAC.equalsIgnoreCase(mac)) {
      String newVal = String(mac) + ":" + String(millis() / 1000);
      prefs.putString(key, newVal);
      Serial.println("[BOND] Updated timestamp for " + String(mac) + " in slot " + String(i));
      break;
    }
  }
  prefs.end();
}

// isBonded() -- returns true if MAC is in our bond list
bool isBonded(const char* mac) {
  if (strlen(mac) == 0) return false;
  prefs.begin("bonds", true);
  char key[7];
  bool found = false;
  for (uint8_t i = 0; i < MAX_BONDS; i++) {
    snprintf(key, sizeof(key), "bond%d", i);
    String val = prefs.getString(key, "");
    if (val.length() == 0) continue;
    int lastColon = val.lastIndexOf(':');
    String storedMAC = val.substring(0, lastColon);
    if (storedMAC.equalsIgnoreCase(mac)) { found = true; break; }
  }
  prefs.end();
  return found;
}

// ---------------------------------------------------------------------------
// BLE server connection callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected      = true;
    bleDisconnectedAt = 0;
    digitalWrite(STATUS_LED_PIN, HIGH);
    strip.clear();
    strip.show();
    currentPeerMAC[0] = '\0'; // clear -- filled by onAuthenticationComplete after pairing
    Serial.print(F("[BLE] Phone connected -- MTU: "));
    Serial.println(BLEDevice::getMTU());
    Serial.println("[BOND] Total bonds stored: " + String(getBondCount()));
  }
  void onDisconnect(BLEServer* pServer) {
    bleConnected      = false;
    bleDisconnectedAt = millis();
    digitalWrite(STATUS_LED_PIN, LOW);
    Serial.println(F("[BLE] Phone disconnected -- BLE off in 1 min"));
    BLEDevice::startAdvertising();
  }
};

// ---------------------------------------------------------------------------
// Buffers for split ssid:/pass: commands
// ---------------------------------------------------------------------------
String pendingSSID = "";
String pendingPass = "";

// ---------------------------------------------------------------------------
// BLE RX callbacks -- commands from phone (all under 20 bytes):
//
//   ssid:NetworkName     buffer SSID (send first)
//   pass:Password        buffer password, save to flash, sync NTP
//   tz:America/Edmonton  change timezone, save, apply on reboot
//   time?                return current local time string
//   info?                return version, settings, uptime, last sync
//   reboot:              reboot the device
//   bright:N             set brightness level 1-10
//   clock12:             switch to 12 hour format
//   clock24:             switch to 24 hour format (default)
//   flip:on              flip display 180 degrees (USB top)
//   flip:off             normal orientation (USB bottom, default)
//   resync:N             set NTP re-sync interval in hours (1-168)
// ---------------------------------------------------------------------------
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    String value = pChar->getValue().c_str();
    value.trim();
    Serial.print(F("[BLE] Received: "));
    Serial.println(value);

    // -- ssid:NetworkName ----------------------------------------------
    if (value.startsWith("ssid:")) {
      pendingSSID = value.substring(5);
      Serial.println("[BLE] SSID buffered: " + pendingSSID);
      bleNotify("ssid_ok -- now send pass:");

    // -- pass:Password -------------------------------------------------
    } else if (value.startsWith("pass:")) {
      pendingPass = value.substring(5);
      Serial.println(F("[BLE] Pass buffered"));
      if (pendingSSID.length() > 0) {
        pendingSSID.toCharArray(wifiSSID, 64);
        pendingPass.toCharArray(wifiPass, 64);
        prefs.begin("clock", false);
        prefs.putString("ssid", String(wifiSSID));
        prefs.putString("pass", String(wifiPass));
        prefs.end();
        Serial.println("[BLE] WiFi saved: " + String(wifiSSID));
        bleNotify("wifi_saved -- rebooting...");
        pendingSSID = "";
        pendingPass = "";
        FadeString(COL_GREEN, "rbt");
        wait(500);
        ESP.restart();
      } else {
        bleNotify("err:send ssid: first");
      }

    // -- tz:Timezone ---------------------------------------------------
    } else if (value.startsWith("tz:")) {
      String newTZ = value.substring(3);
      newTZ.toCharArray(tzName, 40);
      prefs.begin("clock", false);
      prefs.putString("tz", String(tzName));
      prefs.end();
      // myTZ.setLocation() needs active WiFi -- cannot apply while BLE is running
      // Save to flash and reboot -- timezone applied cleanly on next boot
      Serial.println("[BLE] TZ saved: " + String(tzName) + " -- rebooting to apply");
      bleNotify("tz_saved -- rebooting...");
      FadeString(COL_GREEN, "rbt");
      wait(500);
      ESP.restart();

    // -- time? ---------------------------------------------------------
    // Returns time only -- respects hour12 setting -- safe under 20 byte limit
    } else if (value == "time?") {
      if (ntpSynced) {
        String timeStr = myTZ.dateTime(timeFormat());
        Serial.println("[BLE] time? -> " + timeStr);
        bleNotify(timeStr.c_str());
      } else {
        Serial.println(F("[BLE] time? -- no sync yet"));
        bleNotify("err:no sync yet");
      }

    // -- date? ---------------------------------------------------------
    // Returns date and timezone -- separate from time? to stay under 20 byte limit
    } else if (value == "date?") {
      if (ntpSynced) {
        String dateStr = myTZ.dateTime("d/m/Y T");
        Serial.println("[BLE] date? -> " + dateStr);
        bleNotify(dateStr.c_str());
      } else {
        Serial.println(F("[BLE] date? -- no sync yet"));
        bleNotify("err:no sync yet");
      }

    // -- info? ---------------------------------------------------------
    } else if (value == "info?") {
      Serial.println(F("[BLE] info? -- sending device info"));
      bleNotifyLine("ver:0.13.0");
      bleNotifyLine(("ssid:" + String(wifiSSID)).c_str());
      bleNotifyLine(("tz:" + String(tzName)).c_str());
      bleNotifyLine(("bright:" + String(brightLevel)).c_str());
      bleNotifyLine(hour12 ? "format:12h" : "format:24h");
      bleNotifyLine(flipped ? "flip:on" : "flip:off");
      bleNotifyLine(("resync:" + String(resyncHours) + "h").c_str());
      bleNotifyLine(("uptime:" + uptimeString()).c_str());
      if (lastSyncUnix > 0) {
        String syncStr = "sync:" + UTC.dateTime(lastSyncUnix, "H:i d/m");
        bleNotifyLine(syncStr.c_str());
      } else {
        bleNotifyLine("sync:none");
      }
      bleNotify("---");

    // -- reboot: -------------------------------------------------------
    } else if (value == "reboot:") {
      Serial.println(F("[BLE] reboot: -- rebooting in 500ms"));
      bleNotify("rebooting...");
      FadeString(COL_GREEN, "rbt");
      wait(500);
      ESP.restart();

    // -- bright:N ------------------------------------------------------
    } else if (value.startsWith("bright:")) {
      int level = value.substring(7).toInt();
      if (level >= 1 && level <= 10) {
        brightLevel = (uint8_t)level;
        strip.setBrightness(brightLevel * 15);
        strip.show();
        prefs.begin("clock", false);
        prefs.putUChar("bright", brightLevel);
        prefs.end();
        Serial.println("[BLE] Brightness: level " + String(brightLevel) + " (" + String(brightLevel * 15) + "/150)");
        bleNotify("bright_ok");
        FadeString(COL_AMBER, "ok");
      } else {
        Serial.println(F("[BLE] bright: out of range -- must be 1-10"));
        bleNotify("err:bright must be 1-10");
      }

    // -- clock12: / clock24: -------------------------------------------
    } else if (value == "clock12:") {
      hour12 = true;
      prefs.begin("clock", false);
      prefs.putBool("hour12", true);
      prefs.end();
      Serial.println(F("[BLE] Time format: 12h"));
      bleNotify("clock12_ok");
      FadeString(COL_GREEN, "ok");

    } else if (value == "clock24:") {
      hour12 = false;
      prefs.begin("clock", false);
      prefs.putBool("hour12", false);
      prefs.end();
      Serial.println(F("[BLE] Time format: 24h"));
      bleNotify("clock24_ok");
      FadeString(COL_GREEN, "ok");

    // -- flip:on / flip:off --------------------------------------------
    } else if (value == "flip:on") {
      flipped = true;
      prefs.begin("clock", false);
      prefs.putBool("flip", true);
      prefs.end();
      Serial.println(F("[BLE] Display: flipped (USB top)"));
      bleNotify("flip_ok");
      FadeString(COL_GREEN, "ok");

    } else if (value == "flip:off") {
      flipped = false;
      prefs.begin("clock", false);
      prefs.putBool("flip", false);
      prefs.end();
      Serial.println(F("[BLE] Display: normal (USB bottom)"));
      bleNotify("flip_ok");
      FadeString(COL_GREEN, "ok");

    // -- resync:N ------------------------------------------------------
    } else if (value.startsWith("resync:")) {
      int hours = value.substring(7).toInt();
      if (hours >= 1 && hours <= 168) {
        resyncHours = (uint8_t)hours;
        prefs.begin("clock", false);
        prefs.putUChar("resync", resyncHours);
        prefs.end();
        Serial.println("[BLE] Resync interval: " + String(resyncHours) + "h");
        bleNotify(("resync_ok:" + String(resyncHours) + "h").c_str());
      } else {
        Serial.println(F("[BLE] resync: out of range -- must be 1-168"));
        bleNotify("err:resync must be 1-168");
      }

    // -- unknown -------------------------------------------------------
    } else {
      Serial.println(F("[BLE] Unknown command"));
      bleNotify("err:unknown -- valid: ssid: pass: tz: time? date? info? reboot: bright: clock12: clock24: flip:on flip:off resync:");
    }
  }
};

// ---------------------------------------------------------------------------
// SecurityCallbacks -- BLE Numeric Comparison pairing
//
// ESP_IO_CAP_IO mode -- Numeric Comparison
//   Stack generates same 6-digit code on both ESP32 and phone
//   onConfirmPIN() called with the code -- we display it and return true
//   Phone shows same code -- user taps confirm to complete pairing
//   No mismatch possible -- both sides see identical number from stack
//
// onPassKeyRequest() / onPassKeyNotify() -- not used in this mode
// onAuthenticationComplete() -- logs success or failure
// ---------------------------------------------------------------------------
class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() {
    // Not used in Numeric Comparison mode -- logged if called unexpectedly
    Serial.println(F("[BLE][SEC] onPassKeyRequest (unexpected in IO_CAP_IO mode)"));
    return 0;
  }

  void onPassKeyNotify(uint32_t pass_key) {
    // Not used in Numeric Comparison mode -- logged if called unexpectedly
    Serial.println("[BLE][SEC] onPassKeyNotify (unexpected in IO_CAP_IO mode): " + String(pass_key));
  }

  bool onConfirmPIN(uint32_t pass_key) {
    // Numeric Comparison -- stack calls this on BOTH sides with the same number
    // We display it on matrix and auto-confirm our side (return true)
    // Phone displays same number -- user taps confirm on phone to complete pairing
    currentPasskey = pass_key;
    snprintf(pinToShow, sizeof(pinToShow), "%06lu", pass_key);
    Serial.println("[BLE][SEC] onConfirmPIN -- displaying PIN: " + String(pinToShow));
    showPIN = true;
    return true; // auto-confirm our side -- user confirms on phone
  }

  bool onSecurityRequest() {
    Serial.println(F("[BLE] Security request received from phone"));
    return true; // accept pairing request
  }

  void onAuthenticationComplete(int result) {
    // Arduino BLE wrapper passes plain int -- nonzero = success
    if (result) {
      Serial.println(F("[BLE] Pairing SUCCESS -- saving bond"));
      // Get peer address from BLE stack -- best available at auth complete time
      String mac = BLEDevice::getAddress().toString().c_str();
      Serial.println("[BLE] Local address (debug): " + mac);
      // currentPeerMAC set by stack -- save to Preferences
      // If currentPeerMAC is empty, log warning -- bond save skipped
      if (strlen(currentPeerMAC) > 0) {
        saveBond(currentPeerMAC);
      } else {
        Serial.println(F("[BOND] WARNING -- peer MAC not captured, bond not saved to Preferences"));
        Serial.println(F("[BOND] ESP32 BLE stack handles bond internally -- will reconnect without PIN"));
      }
    } else {
      Serial.println(F("[BLE] Pairing FAILED"));
    }
  }
};

// ---------------------------------------------------------------------------
// startBLE() -- start BLE advertising
// BLEDevice::init() called only once -- deinit() corrupts heap on reinit
// Subsequent calls just restart advertising -- server/service/chars stay intact
// WRITE_NR required on ESP32-C3 -- plain WRITE silently drops callbacks
// MTU set to 512 -- Android still caps WRITE_NR at 20 bytes regardless
// Security: Numeric Comparison (ESP_IO_CAP_IO) + MITM + Bond
// ---------------------------------------------------------------------------
void startBLE() {
  if (bleActive) return;
  Serial.println(F("[BLE] Starting..."));

  if (!bleInitialized) {
    // First call only -- init BLE stack, configure security, create server and service
    // Never called again after this -- avoids heap corruption from deinit()/init() cycle
    BLEDevice::init("FingerClock");
    BLEDevice::setMTU(512);

    // Security -- ESP_IO_CAP_IO: Numeric Comparison mode
    // Stack generates passkey, calls onConfirmPIN() on both sides with same number
    // We display it and auto-confirm -- phone displays it and user taps confirm
    // Static allocation -- only created once, never reallocated
    Serial.println(F("[BLE] Configuring security: Numeric Comparison, MITM, Bond"));
    static SecurityCallbacks securityCallbacks;
    static BLESecurity       bleSecurity;
    BLEDevice::setSecurityCallbacks(&securityCallbacks);
    bleSecurity.setCapability(ESP_IO_CAP_IO);
    bleSecurity.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    bleSecurity.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    bleSecurity.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    Serial.println(F("[BLE] Security configured"));

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    BLECharacteristic* pCharRX = pService->createCharacteristic(
      CHAR_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE_NR  // WRITE_NR required on ESP32-C3
    );
    pCharRX->setCallbacks(new RxCallbacks());

    pCharTX = pService->createCharacteristic(
      CHAR_UUID_TX,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharTX->addDescriptor(new BLE2902());
    pCharTX->setValue("FingerClock v0.13.0 ready");

    pService->start();
    bleInitialized = true;
  } else {
    // Subsequent calls -- just restart advertising, server and service stay intact
    Serial.println(F("[BLE] Restarting advertising (already initialized)"));
  }

  BLEDevice::startAdvertising();
  bleActive         = true;
  bleStartedAt      = millis();
  bleDisconnectedAt = 0;
  Serial.println(F("[BLE] Advertising -- connect with nRF Connect or LightBlue"));
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[+] FingerClock v0.13.0 booting..."));

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Load all settings from flash before first strip.show()
  prefs.begin("clock", true);
  String ssid  = prefs.getString("ssid", "");
  brightLevel  = prefs.getUChar("bright",  5);
  hour12       = prefs.getBool("hour12",   false);
  flipped      = prefs.getBool("flip",     false);
  resyncHours  = prefs.getUChar("resync",  100);
  prefs.end();

  Serial.println("[+] Brightness: level " + String(brightLevel) + " (" + String(brightLevel * 15) + "/150)");
  Serial.println("[+] Time format: " + String(hour12 ? "12h" : "24h"));
  Serial.println("[+] Display: " + String(flipped ? "flipped (USB top)" : "normal (USB bottom)"));
  Serial.println("[+] Resync interval: " + String(resyncHours) + "h");

  strip.begin();
  strip.setBrightness(brightLevel * 15);
  strip.show();

  if (ssid.length() == 0) {
    // No credentials -- go straight to BLE config
    Serial.println(F("[!] No WiFi saved -- waiting for BLE config"));
    FadeString(COL_BLUE, "bLE");
    startBLE();
    FadeString(COL_AMBER, "cfg");
  } else {
    // Try WiFi -- auto BLE on 3 failures
    FadeString(COL_RED, "ntp");
    syncNTP();
    if (ntpSynced) {
      FadeString(COL_GREEN, "Up");
    } else {
      // All WiFi attempts failed -- enter BLE config automatically
      FadeString(COL_BLUE, "bLE");
      startBLE();
      FadeString(COL_AMBER, "cfg");
    }
  }
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
  events(); // ezTime background tasks

  unsigned long now = millis();
  static unsigned long lastDraw   = 0;
  static bool          btnHandled = false;

  // -- Periodic NTP re-sync ------------------------------------------------
  // Only when BLE is not active -- shared radio guard
  if (ntpSynced && !bleActive && lastSync > 0) {
    unsigned long resyncMs = (unsigned long)resyncHours * 3600000UL;
    if (now - lastSync >= resyncMs) {
      Serial.println("[+] Periodic NTP resync (" + String(resyncHours) + "h interval)");
      syncNTP();
      if (!ntpSynced) {
        // Periodic resync failed -- enter BLE config
        Serial.println(F("[-] Periodic resync failed -- entering BLE config"));
        FadeString(COL_BLUE, "bLE");
        startBLE();
      }
    }
  }

  // -- PIN display ---------------------------------------------------------
  // showPIN set by onConfirmPIN() -- display here, never inside the callback
  // Returning from onConfirmPIN quickly is required -- display in loop() instead
  if (showPIN) {
    showPIN = false; // clear flag immediately before blocking display
    Serial.println("[PIN] Showing PIN on matrix: " + String(pinToShow));
    ShowString(COL_WHITE, pinToShow);
  }

  // -- Time display --------------------------------------------------------
  if (ntpSynced) {
    if (minuteChanged()) {
      FadeString(COL_BLUE, myTZ.dateTime("dmy").c_str());
      lastDraw = 0;
    }
    if (now - lastDraw >= TIME_REFRESH_MS) {
      FadeString(COL_AMBER, myTZ.dateTime(timeFormat()).c_str());
      lastDraw = now;
    }
  }

  // -- BLE advertising animation + auto-timeout ----------------------------
  if (bleActive && !bleConnected) {
    // Only animate when clock not synced -- avoids conflict with time display
    // When synced, status LED on pin 10 indicates BLE is active
    if (!ntpSynced) radarAnimation();

    // 3 minutes with no connection -- shut BLE off
    if (bleStartedAt > 0 && now - bleStartedAt >= BLE_ADVERTISE_TIMEOUT_MS) {
      Serial.println(F("[BLE] No connection -- timeout, shutting off"));
      BLEDevice::stopAdvertising();
      bleActive    = false;
      bleStartedAt = 0;
      strip.clear();
      strip.show();
    }
    // 1 minute after disconnect -- shut BLE off
    if (bleDisconnectedAt > 0 && now - bleDisconnectedAt >= BLE_DISCONNECT_TIMEOUT_MS) {
      Serial.println(F("[BLE] Post-disconnect timeout -- shutting off"));
      BLEDevice::stopAdvertising();
      bleActive         = false;
      bleDisconnectedAt = 0;
      strip.clear();
      strip.show();
    }
  }

  // -- Button 3s -- restart BLE for config ---------------------------------
  if (digitalRead(BTN_PIN) == LOW) {
    if (btnPressStart == 0) {
      btnPressStart = now;
      btnHandled    = false;
      Serial.println(F("[+] Button held..."));
    }
    if (!btnHandled && now - btnPressStart >= LONG_PRESS_MS) {
      btnHandled = true;
      Serial.println(F("[!] Long press -- restarting BLE"));
      FadeString(COL_BLUE, "bLE");
      if (bleActive) {
        BLEDevice::stopAdvertising();
        bleActive    = false;
        bleConnected = false;
      }
      startBLE();
    }
  } else {
    if (btnPressStart > 0 && !btnHandled) {
      Serial.println(F("[-] Released too early"));
    }
    btnPressStart = 0;
    btnHandled    = false;
  }
}
