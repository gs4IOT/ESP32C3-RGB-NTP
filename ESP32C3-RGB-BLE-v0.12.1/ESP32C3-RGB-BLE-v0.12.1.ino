// ESP32C3 RGB NTP v0.12.1
// Bridge version -- BLE config, WiFi scan with RSSI/encryption, no security yet
// Step 1: BLE replaces WiFiManager -- WiFi used briefly on boot for NTP only
// Step 2: WiFi scan returns SSID, RSSI, encryption type
// Step 3: Button 3s placeholder for bond clear (security comes in v0.13.0)

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
  Serial.println(F("[+] WiFi off -- BLE resuming"));
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
// BLE RX callbacks -- commands from phone:
//
//   wifi:SSID:Password   save credentials, trigger NTP sync
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

    // -- wifi:SSID:Password --------------------------------------------
    if (value.startsWith("wifi:")) {
      String rest = value.substring(5);
      int sep = rest.indexOf(':');
      if (sep > 0) {
        rest.substring(0, sep).toCharArray(wifiSSID, 64);
        rest.substring(sep + 1).toCharArray(wifiPass, 64);
        prefs.begin("clock", false);
        prefs.putString("ssid", String(wifiSSID));
        prefs.putString("pass", String(wifiPass));
        prefs.end();
        Serial.println("[BLE] WiFi saved: " + String(wifiSSID));
        bleNotify("wifi_saved -- syncing NTP...");
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
        bleNotify("err:bad_format -- use wifi:SSID:Password");
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
        bleNotify(("tz_ok:" + String(tzName)).c_str());
        FadeString(COL_GREEN, "ok");
      } else {
        Serial.println(F("[BLE] TZ failed"));
        bleNotify("err:invalid timezone -- check IANA string");
        FadeString(COL_RED, "Err");
      }

    // -- scan ----------------------------------------------------------
    } else if (value == "scan") {
      Serial.println(F("[BLE] WiFi scan started..."));
      FadeString(COL_BLUE, "scn");
      bleNotify("scanning...");

      WiFi.mode(WIFI_STA);
      int found = WiFi.scanNetworks();

      if (found == 0) {
        bleNotify("networks:none");
      } else {
        String chunk = "networks:";
        for (int i = 0; i < found; i++) {
          String enc;
          switch (WiFi.encryptionType(i)) {
            case WIFI_AUTH_OPEN:            enc = "OPEN";     break;
            case WIFI_AUTH_WEP:             enc = "WEP";      break;
            case WIFI_AUTH_WPA_PSK:         enc = "WPA";      break;
            case WIFI_AUTH_WPA2_PSK:        enc = "WPA2";     break;
            case WIFI_AUTH_WPA_WPA2_PSK:    enc = "WPA/2";    break;
            case WIFI_AUTH_WPA2_ENTERPRISE: enc = "WPA2-ENT"; break;
            case WIFI_AUTH_WPA3_PSK:        enc = "WPA3";     break;
            default:                        enc = "UNK";      break;
          }
          String entry = WiFi.SSID(i)
                       + "(" + String(WiFi.RSSI(i)) + "dBm,"
                       + enc + ")";

          // Chunk if approaching BLE 512 byte notify limit
          if (chunk.length() + entry.length() + 1 > 480) {
            Serial.println("[BLE] Scan chunk: " + chunk);
            bleNotify(chunk.c_str());
            delay(100);
            chunk = "networks+:"; // + indicates continuation
          }
          chunk += entry;
          if (i < found - 1) chunk += ",";
        }
        Serial.println("[BLE] Scan chunk: " + chunk);
        bleNotify(chunk.c_str());
      }

      WiFi.scanDelete();
      WiFi.mode(WIFI_OFF);
      FadeString(COL_GREEN, "ok");

    // -- time? ---------------------------------------------------------
    } else if (value == "time?") {
      if (ntpSynced) {
        String t = myTZ.dateTime("H:i:s d/m/Y T");
        bleNotify(t.c_str());
      } else {
        bleNotify("err:no time sync yet -- send wifi: first");
      }

    // -- unknown -------------------------------------------------------
    } else {
      Serial.println(F("[BLE] Unknown command"));
      bleNotify("err:unknown -- valid cmds: wifi: tz: scan time?");
    }
  }
};

// ---------------------------------------------------------------------------
// startBLE() -- init BLE server and start advertising
// WRITE_NR required on ESP32-C3 -- plain WRITE silently drops callbacks
// No security in v0.12.1 -- bonding added in v0.13.0
// ---------------------------------------------------------------------------
void startBLE() {
  if (bleActive) return;
  Serial.println(F("[BLE] Starting..."));

  BLEDevice::init("FingerClock");
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
  pCharTX->setValue("FingerClock v0.12.1 ready");

  pService->start();
  BLEDevice::startAdvertising();
  bleActive = true;
  Serial.println(F("[BLE] Advertising -- connect with nRF Connect or LightBlue"));
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
    // No credentials yet -- start BLE, wait for wifi: command
    Serial.println(F("[!] No WiFi saved -- waiting for BLE config"));
    FadeString(COL_BLUE, "bLE");
    startBLE();
    FadeString(COL_AMBER, "cfg");
  } else {
    // Normal boot -- sync NTP then start BLE
    FadeString(COL_RED, "ntp");
    syncNTP();
    if (ntpSynced) {
      FadeString(COL_GREEN, "Up");
    } else {
      FadeString(COL_RED, "Err");
    }
    startBLE();
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

  // -- Button 3s -- bond clear placeholder (v0.13.0) -----------------------
  if (digitalRead(BTN_PIN) == LOW) {
    if (btnPressStart == 0) {
      btnPressStart = now;
      btnHandled    = false;
      Serial.println(F("[+] Button held..."));
    }
    if (!btnHandled && now - btnPressStart >= LONG_PRESS_MS) {
      btnHandled = true;
      Serial.println(F("[!] Long press -- bond clear active in v0.13.0"));
      FadeString(COL_RED, "CLr");
      FadeString(COL_AMBER, "PAr");
    }
  } else {
    if (btnPressStart > 0 && !btnHandled) {
      Serial.println(F("[-] Released too early"));
    }
    btnPressStart = 0;
    btnHandled    = false;
  }
}
