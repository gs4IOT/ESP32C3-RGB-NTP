// ESP32C3 RGB NTP v0.12.2
// Change 1 of 4: reboot: command, time? Serial output, updated command hint
// Previous fixes carried forward from v0.12.1:
//   - PROPERTY_WRITE_NR required on ESP32-C3 (plain WRITE drops callbacks silently)
//   - ssid: and pass: sent as separate commands to stay under 20 byte BLE write limit
//   - Full flash erase required before first upload (Tools -> Erase All Flash)
//   - scan command removed -- cannot run WiFi scan while BLE active (shared radio)
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
#define TIME_REFRESH_MS 20000
#define FADE_STEP_MS    30
#define FADE_HOLD_MS    200
#define BTN_PIN         9
#define LONG_PRESS_MS   3000
#define STATUS_LED_PIN  10

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
bool               bleActive     = false;
unsigned long      btnPressStart = 0;
bool               ntpSynced     = false;

// BLE auto-timeout -- shuts BLE off if unused
// 3 minutes with no connection, 1 minute after disconnect
#define BLE_ADVERTISE_TIMEOUT_MS  180000
#define BLE_DISCONNECT_TIMEOUT_MS  60000
unsigned long bleStartedAt      = 0; // when BLE advertising began
unsigned long bleDisconnectedAt = 0; // when phone last disconnected

// ---------------------------------------------------------------------------
// wait() -- non-blocking delay with yield()
// ---------------------------------------------------------------------------
void wait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) yield();
}

// ---------------------------------------------------------------------------
// Drawing functions
// ---------------------------------------------------------------------------
void DrawPixel(uint32_t colour, uint8_t x, uint8_t y, uint8_t brightness) {
  uint8_t led = (4 - y) * 5 + (4 - x);
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
// syncNTP() -- connect WiFi briefly, sync time, disconnect
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

  Serial.println("[+] Connecting to: " + ssid);
  digitalWrite(STATUS_LED_PIN, HIGH);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm); // reduce TX power to lower current draw
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    yield();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[-] WiFi failed"));
    FadeString(COL_RED, "Err");
    digitalWrite(STATUS_LED_PIN, LOW);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    return;
  }

  Serial.println(F("[+] WiFi connected -- syncing NTP"));
  waitForSync();
  Serial.println("[+] UTC: " + UTC.dateTime());

  if (myTZ.setLocation(tzName)) {
    Serial.println("[+] TZ OK: " + String(tzName));
    Serial.println("[+] Local: " + myTZ.dateTime());
    ntpSynced = true;
  } else {
    Serial.println(F("[-] TZ failed, using UTC"));
    myTZ = UTC;
    ntpSynced = true;
  }

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  digitalWrite(STATUS_LED_PIN, LOW);
  Serial.println(F("[+] WiFi off -- hold button to start BLE"));
}

// ---------------------------------------------------------------------------
// BLE server connection callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected      = true;
    bleDisconnectedAt = 0; // clear disconnect timer when phone connects
    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.print(F("[BLE] Phone connected -- MTU: "));
    Serial.println(BLEDevice::getMTU());
  }
  void onDisconnect(BLEServer* pServer) {
    bleConnected      = false;
    bleDisconnectedAt = millis(); // start 1 minute shutdown timer
    digitalWrite(STATUS_LED_PIN, LOW);
    Serial.println(F("[BLE] Phone disconnected -- BLE off in 1 min"));
    BLEDevice::startAdvertising(); // keep advertising for 1 min in case phone reconnects
  }
};

// ---------------------------------------------------------------------------
// Buffers for split ssid:/pass: commands
// Android BLE ignores MTU negotiation for WRITE_NR and caps at 20 bytes
// Sending ssid: and pass: as separate writes keeps each under 20 bytes
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
//   reboot:              reboot the device
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
    // Triggers save + NTP sync once both ssid and pass are received
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
    // Tries to apply immediately -- if it fails WiFi may be required
    // Falls back to saving for next reboot
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
        Serial.println("[BLE] time? -> " + timeStr); // v0.12.2: added Serial output
        bleNotify(timeStr.c_str());
      } else {
        Serial.println(F("[BLE] time? -- no sync yet"));
        bleNotify("err:no time sync yet -- send ssid: and pass: first");
      }

    // -- reboot: -------------------------------------------------------
    // Useful after tz: change since timezone requires reboot to apply
    } else if (value == "reboot:") {
      Serial.println(F("[BLE] reboot: received -- rebooting in 500ms"));
      bleNotify("rebooting...");
      wait(500);
      ESP.restart();

    // -- unknown -------------------------------------------------------
    } else {
      Serial.println(F("[BLE] Unknown command"));
      bleNotify("err:unknown -- valid: ssid: pass: tz: time? reboot:");
    }
  }
};

// ---------------------------------------------------------------------------
// startBLE() -- init BLE server and start advertising
// WRITE_NR required on ESP32-C3 -- plain WRITE silently drops callbacks
// MTU set to 512 -- default 20 byte MTU truncates long wifi: commands
// No security yet -- planned for a future release
// ---------------------------------------------------------------------------
void startBLE() {
  if (bleActive) return;
  Serial.println(F("[BLE] Starting..."));

  BLEDevice::init("FingerClock");
  BLEDevice::setMTU(512); // must be set before createServer()
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
  pCharTX->setValue("FingerClock v0.12.2 ready");

  pService->start();
  BLEDevice::startAdvertising();
  bleActive         = true;
  bleStartedAt      = millis(); // start 3 minute no-connection timeout
  bleDisconnectedAt = 0;        // clear disconnect timer
  Serial.println(F("[BLE] Advertising -- connect with nRF Connect or LightBlue"));
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[+] FingerClock v0.12.2 booting..."));

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  strip.begin();
  strip.setBrightness(20);
  strip.show();

  prefs.begin("clock", true);
  String ssid = prefs.getString("ssid", "");
  prefs.end();

  if (ssid.length() == 0) {
    // No credentials yet -- start BLE, wait for ssid:/pass: commands
    Serial.println(F("[!] No WiFi saved -- waiting for BLE config"));
    FadeString(COL_BLUE, "bLE");
    startBLE();
    FadeString(COL_AMBER, "cfg");
  } else {
    // Normal boot -- sync NTP, then both radios off
    // BLE only starts again on button 3s hold
    FadeString(COL_RED, "ntp");
    syncNTP();
    if (ntpSynced) {
      FadeString(COL_GREEN, "Up");
    } else {
      FadeString(COL_RED, "Err");
    }
    // No startBLE() here -- radios stay off until button pressed
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

  // -- Time display --------------------------------------------------------
  if (ntpSynced) {
    if (minuteChanged()) {
      FadeString(COL_BLUE, myTZ.dateTime("dmy").c_str());
      lastDraw = 0;
    }
    if (now - lastDraw >= TIME_REFRESH_MS) {
      FadeString(COL_AMBER, myTZ.dateTime("H:i").c_str());
      lastDraw = now;
    }
  }

  // -- BLE auto-timeout ----------------------------------------------------
  if (bleActive && !bleConnected) {
    // 3 minutes with no connection -- shut BLE off
    if (bleStartedAt > 0 && now - bleStartedAt >= BLE_ADVERTISE_TIMEOUT_MS) {
      Serial.println(F("[BLE] No connection -- timeout, shutting off"));
      BLEDevice::deinit(true);
      bleActive    = false;
      pServer      = nullptr;
      pCharTX      = nullptr;
      bleStartedAt = 0;
    }
    // 1 minute after disconnect -- shut BLE off
    if (bleDisconnectedAt > 0 && now - bleDisconnectedAt >= BLE_DISCONNECT_TIMEOUT_MS) {
      Serial.println(F("[BLE] Post-disconnect timeout -- shutting off"));
      BLEDevice::deinit(true);
      bleActive         = false;
      pServer           = nullptr;
      pCharTX           = nullptr;
      bleDisconnectedAt = 0;
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
      // Deinit fully so startBLE() can reinitialise from scratch
      // bleActive must be reset to false before deinit() or startBLE() silently returns
      if (bleActive) {
        BLEDevice::deinit(true);
        bleActive    = false; // must reset before startBLE() -- flag guards re-init
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
