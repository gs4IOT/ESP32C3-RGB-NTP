// ESP32C3 RGB NTP v0.12.1
// BLE basic version -- BLE config, no WiFi scan with RSSI/encryption, no security yet
// Main issue is that radio interference prevents us from runing both system simultaneously
// Key fixes of the BLE issues found:
//   - PROPERTY_WRITE_NR required on ESP32-C3 (plain WRITE drops callbacks silently)
//   - ssid: and pass: sent as separate commands to stay under 20 byte BLE write limit
//   - Full flash erase required before first upload (Tools -> Erase All Flash)

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

// Buffers for split ssid:/pass: commands
// Android BLE caps WRITE_NR at 20 bytes regardless of MTU negotiation
// Sending ssid: and pass: separately keeps each write under 20 bytes
String pendingSSID = "";
String pendingPass = "";

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
// syncNTP() -- stop BLE, connect WiFi, sync time, disconnect, restart BLE
// BLE and WiFi share the same radio on ESP32-C3 -- must not run together
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

  // Stop BLE advertising before WiFi -- shared radio, must not transmit together
  // Using stopAdvertising instead of deinit -- deinit blocks for up to 60s
  if (bleActive) {
    Serial.println(F("[+] Pausing BLE before WiFi..."));
    BLEDevice::stopAdvertising();
    bleConnected = false;
    // Yield briefly to let BLE stack finish any in-flight packets
    unsigned long pause = millis();
    while (millis() - pause < 500) yield();
  }

  Serial.println("[+] Connecting to: " + ssid);
  digitalWrite(STATUS_LED_PIN, HIGH);
  WiFi.mode(WIFI_STA);
 WiFi.setTxPower(WIFI_POWER_11dBm); // reduce TX power to lower current draw
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {   /// ???
    yield();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[-] WiFi failed"));
    FadeString(COL_RED, "Err");
    digitalWrite(STATUS_LED_PIN, LOW);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    startBLE(); // restart BLE even after failure
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
  // Yield briefly before restarting BLE
  unsigned long pause = millis();
  while (millis() - pause < 300) yield();
  Serial.println(F("[+] WiFi off -- start BLE holding the button"));
}

// ---------------------------------------------------------------------------
// BLE server connection callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    bleConnected = true;
    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.println(F("[BLE] Phone connected"));
  }
  void onDisconnect(BLEServer* pServer) {
    bleConnected = false;
    digitalWrite(STATUS_LED_PIN, LOW);
    Serial.println(F("[BLE] Phone disconnected"));
    BLEDevice::startAdvertising();
  }
};

// ---------------------------------------------------------------------------
// BLE RX callbacks -- commands from phone (all kept under 20 bytes):
//
//   ssid:NetworkName     buffer SSID (send first)
//   pass:Password        buffer password, save + sync NTP
//   tz:America/Edmonton  change timezone, save, apply immediately
//   scan                 return WiFi networks with RSSI and encryption
//   time?                return current local time string
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
      bleNotify("ssid_ok");

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
        bleNotify("wifi_saved -- rebooting");
        pendingSSID = "";
        pendingPass = "";
        FadeString(COL_GREEN, "rbt");
        unsigned long pause = millis();
        while (millis() - pause < 500) yield(); // let notify send before reboot
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
      if (myTZ.setLocation(tzName)) {
        Serial.println("[BLE] TZ set: " + String(tzName));
        bleNotify("tz_ok");
        FadeString(COL_GREEN, "ok");
      } else {
        Serial.println(F("[BLE] TZ failed"));
        bleNotify("tz_err");
        FadeString(COL_RED, "Err");
      }

    // -- scan should be removed couldnt run both radio at the same time ---
    } else if (value == "scan") {
      Serial.println(F("[BLE] WiFi scan started..."));
      FadeString(COL_BLUE, "scn");
      bleNotify("scanning...");

      // Pause BLE advertising before scan -- shared radio
      if (bleActive) {
        BLEDevice::stopAdvertising();
        bleConnected = false;
        unsigned long pause = millis();
        while (millis() - pause < 500) yield();
      }

      WiFi.mode(WIFI_STA);
      int found = WiFi.scanNetworks();

      WiFi.mode(WIFI_OFF);
      // Yield briefly before restarting BLE
      unsigned long pause = millis();
      while (millis() - pause < 500) yield();
      startBLE(); // restart BLE before sending results

      if (found == 0) {
        bleNotify("networks:none");
      } else {
        String chunk = "networks:";
        for (int i = 0; i < found; i++) {
          String enc;
          switch (WiFi.encryptionType(i)) {
            case WIFI_AUTH_OPEN:            enc = "OPEN";  break;
            case WIFI_AUTH_WEP:             enc = "WEP";   break;
            case WIFI_AUTH_WPA_PSK:         enc = "WPA";   break;
            case WIFI_AUTH_WPA2_PSK:        enc = "WPA2";  break;
            case WIFI_AUTH_WPA_WPA2_PSK:    enc = "WPA/2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: enc = "ENT";   break;
            case WIFI_AUTH_WPA3_PSK:        enc = "WPA3";  break;
            default:                        enc = "UNK";   break;
          }
          String entry = WiFi.SSID(i)
                       + "(" + String(WiFi.RSSI(i)) + ","
                       + enc + ")";
          if (chunk.length() + entry.length() + 1 > 480) {
            Serial.println("[BLE] Scan chunk: " + chunk);
            bleNotify(chunk.c_str());
            unsigned long chunkPause = millis();
            while (millis() - chunkPause < 100) yield();
            chunk = "nets+:";
          }
          chunk += entry;
          if (i < found - 1) chunk += ",";
        }
        Serial.println("[BLE] Scan chunk: " + chunk);
        bleNotify(chunk.c_str());
      }
      WiFi.scanDelete();
      FadeString(COL_GREEN, "ok");

    // -- time? ---------------------------------------------------------
    } else if (value == "time?") {
      if (ntpSynced) {
        bleNotify(myTZ.dateTime("H:i:s d/m/Y T").c_str());
      } else {
        bleNotify("err:not synced yet");
      }

    // -- unknown -------------------------------------------------------
    } else {
      Serial.println(F("[BLE] Unknown command"));
      bleNotify("err:unknown cmd");
    }
  }
};

// ---------------------------------------------------------------------------
// startBLE() -- init BLE server and start advertising
// WRITE_NR required on ESP32-C3 -- plain WRITE silently drops callbacks
// No security in v0.12.1 -- will be added later in next version
// ---------------------------------------------------------------------------
void startBLE() {
  if (bleActive) return;
  Serial.println(F("[BLE] Starting..."));

  BLEDevice::init("FingerClock");
  BLEDevice::setMTU(512);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic* pCharRX = pService->createCharacteristic(
    CHAR_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE_NR // WRITE_NR required on ESP32-C3
  );
  pCharRX->setCallbacks(new RxCallbacks());

  pCharTX = pService->createCharacteristic(
    CHAR_UUID_TX,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharTX->addDescriptor(new BLE2902());
  pCharTX->setValue("FingerClock v0.12.1");

  pService->start();
  BLEDevice::startAdvertising();
  bleActive = true;
  Serial.println(F("[BLE] Advertising"));
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[+] FingerClock v0.12.1 booting..."));

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
    // Normal boot -- sync NTP (BLE starts inside syncNTP after WiFi done)
    FadeString(COL_RED, "ntp");
    syncNTP();
    if (ntpSynced) {
      FadeString(COL_GREEN, "Up");
    } else {
      FadeString(COL_RED, "Err");
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

  // -- Button 3s -- for entering the BLE after NTP synced -----------------------
  if (digitalRead(BTN_PIN) == LOW) {
    if (btnPressStart == 0) {
      btnPressStart = now;
      btnHandled    = false;
      Serial.println(F("[+] Button held..."));
    }
    if (!btnHandled && now - btnPressStart >= LONG_PRESS_MS) {
      btnHandled = true;
      Serial.println(F("[!] Long press -- restart BLE "));
      FadeString(COL_RED, "CLr");
      FadeString(COL_AMBER, "PAr");
      startBLE();   // restart BLE after button detected
    }
  } else {
    if (btnPressStart > 0 && !btnHandled) {
      Serial.println(F("[-] Released too early"));
    }
    btnPressStart = 0;
    btnHandled    = false;
  }
}
