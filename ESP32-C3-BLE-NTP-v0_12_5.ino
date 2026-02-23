// ESP32-C3 BLE NTP v0.12.5
// Changes from v0.12.4:
//   - chaseAnimation(colour) -- three pixels chasing during WiFi connect attempts
//   - radarAnimation() -- rings expanding from centre during BLE advertising
//   - WiFi retry loop -- 3 attempts, show attempt number, animate per attempt
//   - Auto BLE config mode after 3 WiFi failures -- no manual button press needed
// Carried forward from v0.12.4:
//   - info? BLE command, resync:N, periodic NTP re-sync
//   - bright:N, clock12:, clock24:, flip:on/off BLE commands
//   - bleActive flag comments on all deinit() paths
//   - reboot: BLE command, time? Serial output
//   - PROPERTY_WRITE_NR required on ESP32-C3 (plain WRITE drops callbacks silently)
//   - ssid: and pass: sent as separate commands to stay under 20 byte BLE write limit
//   - Full flash erase required before first upload (Tools -> Erase All Flash)
//   - BLE only starts on first boot (no credentials) or button 3s hold
//   - After NTP sync WiFi and BLE both off -- radios idle until button pressed

#include <Adafruit_NeoPixel.h>
#include <ezTime.h>
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
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
bool               bleActive     = false; // MUST be reset to false after every deinit()
                                          // or startBLE() silently returns and BLE never starts
unsigned long      btnPressStart = 0;
bool               ntpSynced     = false;

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

  if (ssid.length() == 0) {
    Serial.println(F("[-] No WiFi credentials -- send via BLE first"));
    FadeString(COL_RED, "nO");
    return;
  }

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
// BLE server connection callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected      = true;
    bleDisconnectedAt = 0;
    digitalWrite(STATUS_LED_PIN, HIGH);
    strip.clear(); // clear radar animation when phone connects
    strip.show();
    Serial.print(F("[BLE] Phone connected -- MTU: "));
    Serial.println(BLEDevice::getMTU());
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
        bleNotify("wifi_saved -- syncing NTP...");
        pendingSSID = "";
        pendingPass = "";
        FadeString(COL_BLUE, "ntp");
        syncNTP();
        if (ntpSynced) {
          FadeString(COL_GREEN, "ok");
          bleNotify("sync_done");
        } else {
          FadeString(COL_RED, "Err");
          bleNotify("sync_failed -- check SSID and password");
        }
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
      if (myTZ.setLocation(tzName)) {
        Serial.println("[BLE] TZ set: " + String(tzName));
        bleNotify("tz_ok");
        FadeString(COL_GREEN, "ok");
      } else {
        Serial.println("[BLE] TZ saved (applies on reboot): " + String(tzName));
        bleNotify("tz_saved -- reboot to apply");
        FadeString(COL_GREEN, "ok");
      }

    // -- time? ---------------------------------------------------------
    } else if (value == "time?") {
      if (ntpSynced) {
        String timeStr = myTZ.dateTime("H:i:s d/m/Y T");
        Serial.println("[BLE] time? -> " + timeStr);
        bleNotify(timeStr.c_str());
      } else {
        Serial.println(F("[BLE] time? -- no sync yet"));
        bleNotify("err:no sync yet");
      }

    // -- info? ---------------------------------------------------------
    } else if (value == "info?") {
      Serial.println(F("[BLE] info? -- sending device info"));
      bleNotifyLine("ver:0.12.5");
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
      bleNotify("err:unknown -- valid: ssid: pass: tz: time? info? reboot: bright: clock12: clock24: flip:on flip:off resync:");
    }
  }
};

// ---------------------------------------------------------------------------
// startBLE() -- init BLE server and start advertising
// WRITE_NR required on ESP32-C3 -- plain WRITE silently drops callbacks
// MTU set to 512 -- Android still caps WRITE_NR at 20 bytes regardless
// No security yet -- planned for a future release
// ---------------------------------------------------------------------------
void startBLE() {
  // bleActive guards against double-init
  // IMPORTANT: bleActive must be false before calling this -- if deinit() was
  // called but bleActive was not reset, this silently returns and BLE never starts
  if (bleActive) return;
  Serial.println(F("[BLE] Starting..."));

  BLEDevice::init("FingerClock");
  BLEDevice::setMTU(512);
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
  pCharTX->setValue("FingerClock v0.12.5 ready");

  pService->start();
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
  Serial.println(F("\n[+] FingerClock v0.12.5 booting..."));

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
      BLEDevice::deinit(true);
      bleActive    = false; // MUST reset -- see bleActive comment in globals
      pServer      = nullptr;
      pCharTX      = nullptr;
      bleStartedAt = 0;
      strip.clear();
      strip.show();
    }
    // 1 minute after disconnect -- shut BLE off
    if (bleDisconnectedAt > 0 && now - bleDisconnectedAt >= BLE_DISCONNECT_TIMEOUT_MS) {
      Serial.println(F("[BLE] Post-disconnect timeout -- shutting off"));
      BLEDevice::deinit(true);
      bleActive         = false; // MUST reset -- see bleActive comment in globals
      pServer           = nullptr;
      pCharTX           = nullptr;
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
        BLEDevice::deinit(true);
        bleActive    = false; // MUST reset -- see bleActive comment in globals
        bleConnected = false;
        pServer      = nullptr;
        pCharTX      = nullptr;
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
