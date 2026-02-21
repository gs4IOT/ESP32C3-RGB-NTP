// ESP32C3 RGB NTP v0.11.1
// Step 1: Performance fixes (strip.show, millis loop, const char*, hex colours, r/g/b shadowing)
// Step 2: Non-blocking wait() with yield() replaces delay()
// Step 3: Timezone portal + Preferences — saves chosen timezone to flash
// Step 4: Long press button (pin 9, 3s) opens WiFiManager portal
// Step 5: Status LED (pin 10) lights while WiFi connecting / portal open
// Step 6: Timezone list moved to timezones.h — keeps main file clean

#include <Adafruit_NeoPixel.h>
#include <ezTime.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "font.h"
#include "timezones.h"

#define CHAR_WIDTH  5
#define CHAR_HEIGHT 5
#define LED_PIN     8
#define LED_COUNT   25

#define TIME_REFRESH_MS 20000 // how often to redraw time (ms)
#define FADE_STEP_MS    30    // delay between each brightness step (ms)
#define FADE_HOLD_MS    200   // how long character stays at full brightness (ms)
#define PORTAL_BTN_PIN  9     // built-in button — hold 3s to open WiFi portal
#define LONG_PRESS_MS   3000  // how long to hold button to trigger portal (ms)
#define STATUS_LED_PIN  10    // built-in blue LED — on while connecting / portal open

// Colour constants — plain hex, safe at global scope
const uint32_t COL_RED   = 0xFF0000;
const uint32_t COL_GREEN = 0x00FF00;
const uint32_t COL_BLUE  = 0x0000FF;
const uint32_t COL_AMBER = 0x64C800;

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;
Timezone myTZ;

char tzName[40] = "America/Edmonton"; // default (MST/MDT) — overwritten by saved prefs
char apName[32] = "FingerClock";        // AP name shown in WiFi list — saved to flash
char apPass[32] = "NTP323";         // AP password — saved to flash
unsigned long btnPressStart = 0;      // tracks when button was pressed for long press detection

// ─────────────────────────────────────────────────────────────────────────────
// wait() — non-blocking delay that feeds the ESP32 watchdog via yield()
// Prevents watchdog resets during long waits without blocking background tasks
// ─────────────────────────────────────────────────────────────────────────────
void wait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    yield(); // feed watchdog and run background RTOS tasks
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawPixel() — sets a single LED by x,y with brightness scaling (0-100)
// Uses r,g,b locals to avoid shadowing global colour constants
// ─────────────────────────────────────────────────────────────────────────────
void DrawPixel(uint32_t colour, uint8_t x, uint8_t y, uint8_t brightness) {
  uint8_t led = (4 - y) * 5 + (4 - x); // 0,0 is bottom left
  uint8_t r = ((colour >> 16) & 0xff) * brightness / 100;
  uint8_t g = ((colour >>  8) & 0xff) * brightness / 100;
  uint8_t b = ( colour        & 0xff) * brightness / 100;
  strip.setPixelColor(led, r, g, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawChar() — renders character from font[] onto matrix
// strip.show() called once after all pixels set — not inside the loop
// Based on: https://jared.geek.nz/2014/01/custom-fonts-for-microcontrollers
// ─────────────────────────────────────────────────────────────────────────────
void DrawChar(uint32_t colour, char c, uint8_t brightness) {
  c = c & 0x7F;
  if (c < ' ') { c = 0; } else { c -= ' '; }
  const uint8_t* chr = font[(uint8_t)c];
  for (uint8_t x = 0; x < CHAR_WIDTH; x++) {
    for (uint8_t y = 0; y < CHAR_HEIGHT; y++) {
      if (chr[x] >> 2 & (1 << y)) {
        DrawPixel(colour, x, y, brightness);
      }
    }
  }
  strip.show(); // once after full character — not per pixel
}

// ─────────────────────────────────────────────────────────────────────────────
// FadeChar() — fades a character in, holds, then fades out
// Uses wait() instead of delay() — keeps watchdog fed during animation
// ─────────────────────────────────────────────────────────────────────────────
void FadeChar(uint32_t colour, char c) {
  for (uint8_t br = 20; br <= 100; br += 5) { DrawChar(colour, c, br); wait(FADE_STEP_MS); }
  wait(FADE_HOLD_MS);
  for (uint8_t br = 100; br > 0; br -= 5)  { DrawChar(colour, c, br); wait(FADE_STEP_MS); }
  strip.clear();
  strip.show();
}

// ─────────────────────────────────────────────────────────────────────────────
// FadeString() — fades each character in sequence
// Uses const char* to avoid Arduino String heap fragmentation
// ─────────────────────────────────────────────────────────────────────────────
void FadeString(uint32_t colour, const char* s) {
  for (int i = 0; s[i] != '\0'; i++) {
    FadeChar(colour, s[i]);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// GetNTP() — connects WiFi, syncs NTP, sets timezone
// forcePortal: if true, clears saved WiFi so portal always opens
// Timezone selectable via dropdown in WiFiManager portal, saved to flash
// ─────────────────────────────────────────────────────────────────────────────
void GetNTP(bool forcePortal) {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setDebugOutput(false);

  // Load previously saved settings from flash
  prefs.begin("clock", false);
  String saved = prefs.getString("tz", "America/Edmonton");
  saved.toCharArray(tzName, 40);
  String savedAP = prefs.getString("apName", "FingerClock");
  savedAP.toCharArray(apName, 32);
  String savedPass = prefs.getString("apPass", "NTP323");
  savedPass.toCharArray(apPass, 32);
  prefs.end();
  Serial.println("[+] Loaded timezone: " + String(tzName));
  Serial.println("[+] AP name: " + String(apName));

  // If long press triggered, wipe saved WiFi so portal always opens
  if (forcePortal) {
    Serial.println(F("[!] Force portal — clearing WiFi settings"));
    wm.resetSettings();
  }

  // Timezone dropdown shown in the WiFiManager captive portal
  // Full list defined in timezones.h — add/remove entries there
  WiFiManagerParameter tzParam(TZ_OPTIONS_HTML);
  wm.addParameter(&tzParam);

  // Custom AP name and password fields shown in the portal
  WiFiManagerParameter apNameParam("apName", "AP Name", apName, 32);
  WiFiManagerParameter apPassParam("apPass", "AP Password", apPass, 32);
  wm.addParameter(&apNameParam);
  wm.addParameter(&apPassParam);

  // Save all settings to flash when user submits the portal
  wm.setSaveParamsCallback([&]() {
    // Save timezone
    String chosen = wm.server->arg("tz");
    if (chosen.length() > 0) {
      chosen.toCharArray(tzName, 40);
    }
    // Save AP name
    String newAP = wm.server->arg("apName");
    if (newAP.length() > 0) {
      newAP.toCharArray(apName, 32);
    }
    // Save AP password (minimum 8 chars required for WPA2)
    String newPass = wm.server->arg("apPass");
    if (newPass.length() >= 8) {
      newPass.toCharArray(apPass, 32);
    } else if (newPass.length() > 0) {
      Serial.println(F("[-] AP password too short — must be 8+ chars, keeping old password"));
    }
    prefs.begin("clock", false);
    prefs.putString("tz",     String(tzName));
    prefs.putString("apName", String(apName));
    prefs.putString("apPass", String(apPass));
    prefs.end();
    Serial.println("[+] Settings saved — TZ: " + String(tzName) + " AP: " + String(apName));
  });

  // Status LED on while connecting / portal is open
  digitalWrite(STATUS_LED_PIN, HIGH);
  bool res = wm.autoConnect(apName, apPass);
  digitalWrite(STATUS_LED_PIN, LOW);
  if (!res) {
    Serial.println(F("[-] WiFi failed"));
  } else {
    Serial.println(F("[+] Connected to WiFi"));
    Serial.print(F("[+] IP: "));   Serial.println(WiFi.localIP());
    Serial.print(F("[+] RSSI: ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
    Serial.println(F("[+] Syncing NTP..."));
    waitForSync();
    Serial.println("[+] UTC: " + UTC.dateTime());
    if (myTZ.setLocation(tzName)) {
      Serial.println("[+] Timezone OK: " + String(tzName));
      Serial.println("[+] Local: " + myTZ.dateTime());
    } else {
      Serial.println(F("[-] Timezone failed, using UTC"));
      myTZ = UTC;
    }
  }
  WiFi.disconnect();
}

void setup() {
  Serial.begin(115200);

  pinMode(PORTAL_BTN_PIN, INPUT_PULLUP); // button between pin 9 and GND
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  strip.begin();
  strip.setBrightness(20);
  strip.show();

  FadeString(COL_RED, "WiFi");
  GetNTP(false);
  FadeString(COL_GREEN, "Up");
}

void loop() {
  events();

  unsigned long now = millis();
  static unsigned long lastDraw = 0;

  // ── Time display ──────────────────────────────────────────────────────────
  if (minuteChanged()) {
    FadeString(COL_BLUE, myTZ.dateTime("dmy").c_str());
    lastDraw = 0;
  }
  if (now - lastDraw >= TIME_REFRESH_MS) {
    FadeString(COL_AMBER, myTZ.dateTime("H:i").c_str());
    lastDraw = now;
  }

  // ── Long press detection ──────────────────────────────────────────────────
  // Hold button for LONG_PRESS_MS to open WiFiManager portal
  if (digitalRead(PORTAL_BTN_PIN) == LOW) {
    if (btnPressStart == 0) {
      btnPressStart = now;
      Serial.println(F("[+] Button held..."));
    }
    if (now - btnPressStart >= LONG_PRESS_MS) {
      digitalWrite(STATUS_LED_PIN, HIGH); // solid blue = portal activated
      Serial.println(F("[!] Portal activated"));
      FadeString(COL_RED, "rSt");
      btnPressStart = 0;
      GetNTP(true);
      digitalWrite(STATUS_LED_PIN, LOW);
      FadeString(COL_GREEN, "ok");
      lastDraw = 0;
    }
  } else {
    if (btnPressStart > 0) {
      Serial.println(F("[-] Released too early"));
    }
    btnPressStart = 0;
  }
}
