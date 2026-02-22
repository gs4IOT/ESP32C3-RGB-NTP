# ESP32C3 RGB NTP
**Version 0.12.1**

A WiFi-synced NTP clock based on a PV Electronics single digit Nixie clock design, running on the 01Space ESP32-C3FH4-RGB board with a 5x5 WS2812B NeoPixel matrix.

Displays time every 20 seconds and date in DDMMYY format every minute. Uses ezTime for NTP sync and a custom 5x5 pixel font based on [custom-fonts-for-microcontrollers](https://jared.geek.nz/2014/jan/custom-fonts-for-microcontrollers).

---

## Hardware

| Component | Detail |
|---|---|
| Board | 01Space ESP32-C3FH4-RGB |
| Display | 25x WS2812B NeoPixel (5x5 matrix), pin 8 |
| Button | Built-in button, pin 9 |
| Status LED | Built-in blue LED, pin 10 |

---

## Features

- Syncs time via NTP on every boot, then drops WiFi
- Displays time in amber every 20 seconds
- Displays date in blue on every minute change
- BLE always on for configuration -- no portal, no AP hosted
- WiFi credentials and timezone sent securely over BLE
- WiFi scan returns nearby networks with RSSI and encryption type
- Timezone saved to flash, survives reboots
- Status LED lights while connecting or phone is connected over BLE
- Non-blocking animations -- no delay(), watchdog safe
- WRITE_NR characteristic required on ESP32-C3 for reliable BLE writes

---

## Required Libraries

Install via Arduino IDE -> Tools -> Manage Libraries:

| Library | Author |
|---|---|
| Adafruit NeoPixel | Adafruit |
| ezTime | Rop Gonggrijp |

The following are built into the ESP32 Arduino core -- no install needed:

- WiFi
- Preferences
- BLEDevice / BLEServer / BLEUtils / BLE2902

Note: Do NOT install the separate "ESP32 BLE Arduino" library -- it conflicts with the core BLE headers on ESP32 core v2.x+.

---

## File Structure

```
ESP32C3-RGB-BLE.ino   -- main logic
font.h                -- 5x5 pixel font
```

---

## Board Settings (Arduino IDE)

| Setting | Value |
|---|---|
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |
| Flash Size | 4MB |
| Partition Scheme | Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS) |

The Minimal SPIFFS partition scheme is required -- default 4MB gives only 1.3MB app space which is too small for BLE + WiFi together.

---

## First Flash

Before first flash and after any BLE issues, enable full flash erase:

Arduino IDE -> Tools -> Erase All Flash Before Sketch Upload -> Enabled

Upload once, then set it back to Disabled. Skipping this step causes duplicate BLE services and silent write failures.

---

## First Boot

1. Power on -- matrix shows "bLE" then "cfg"
2. Open LightBlue or nRF Connect on your phone
3. Scan and connect to "FingerClock"
4. Find the WRITE characteristic (abcd1234...) and send as UTF-8:
   ```
   wifi:YourSSID:YourPassword
   ```
5. Board connects to WiFi, syncs NTP, disconnects -- matrix shows "ntp" then "ok"
6. Send your timezone:
   ```
   tz:America/Edmonton
   ```
7. Matrix shows "ok" -- clock is running

On every subsequent boot the board connects WiFi automatically, syncs NTP, drops WiFi, then starts BLE.

---

## BLE Commands

Connect with LightBlue or nRF Connect. Write to the WRITE characteristic (abcd1234...) as UTF-8 string. Responses come back on the NOTIFY characteristic (abcd1235...).

| Command | Example | Response |
|---|---|---|
| Save WiFi + sync | wifi:MySSID:MyPassword | wifi_saved / sync_done / sync_failed |
| Set timezone | tz:America/Edmonton | tz_ok:America/Edmonton |
| Scan WiFi networks | scan | networks:SSID(-65dBm,WPA2),... |
| Get current time | time? | 16:23:45 21/02/2026 MST |

### Scan output format

```
networks:HomeWiFi(-65dBm,WPA2),Neighbours(-72dBm,WPA2),OpenNet(-80dBm,OPEN)
```

If there are many networks a second message starting with "networks+:" contains the overflow.

### Signal strength guide

| RSSI | Quality |
|---|---|
| -30 to -50 dBm | Excellent |
| -50 to -65 dBm | Good |
| -65 to -75 dBm | Fair |
| -75 to -90 dBm | Weak |

---

## Button

| Action | Result |
|---|---|
| Hold 3 seconds | Placeholder -- bond clear active in v0.13.0 |

---

## Resetting Saved Settings

To wipe all saved WiFi and timezone data, add temporarily to the top of setup(), flash once, then remove:

```cpp
prefs.begin("clock", false);
prefs.clear();
prefs.end();
```

Or use Arduino IDE: Tools -> Erase All Flash Before Sketch Upload -> Enabled, upload once, disable again.

---

## Customisation

All timing constants are at the top of the sketch:

| Constant | Default | Description |
|---|---|---|
| TIME_REFRESH_MS | 20000 | How often time redraws (ms) |
| FADE_STEP_MS | 30 | Speed of fade animation (ms) |
| FADE_HOLD_MS | 200 | How long character stays lit (ms) |
| LONG_PRESS_MS | 3000 | Button hold time (ms) |

To add timezones for reference, edit timezones.h. Values must be valid IANA timezone strings -- full list at [Wikipedia](https://en.wikipedia.org/wiki/List_of_tz_database_time_zones).

---

## Serial Monitor

Connect at 115200 baud to see status output:

```
[+] FingerClock v0.12.1 booting...
[+] Connecting to: HomeWiFi
[+] WiFi connected -- syncing NTP
[+] UTC: Saturday, 21-Feb-2026 08:03:18 UTC
[+] TZ OK: America/Edmonton
[+] Local: Saturday, 21-Feb-2026 01:03:18 MST
[+] WiFi off -- BLE resuming
[BLE] Advertising -- connect with nRF Connect or LightBlue
[BLE] Phone connected
[BLE] Received: time?
[BLE] Phone disconnected
```

---

## Troubleshooting

**BLE writes not reaching the board (onWrite never fires)**
Do a full flash erase -- Tools -> Erase All Flash Before Sketch Upload -> Enabled, upload, disable. Corrupted NVS state from multiple re-flashes silently drops BLE callbacks.

**Duplicate services in LightBlue / nRF Connect**
Same cause as above -- full erase fixes it. Also clear the app cache on your phone (Settings -> Apps -> LightBlue -> Clear Cache).

**Do not install "ESP32 BLE Arduino" from library manager**
It conflicts with BLE headers built into the ESP32 core on v2.x+. If installed, remove it.

**WRITE vs WRITE_NR**
The RX characteristic uses WRITE_NR (write without response). In LightBlue this is the default Write button. In nRF Connect use the write without response option if available.

---

## Changelog

### v0.12.1
- BLE replaces WiFiManager entirely -- no AP hosted, no captive portal
- WiFi used on boot only for NTP sync, then disabled
- BLE commands: wifi: tz: scan time?
- WiFi scan returns SSID, RSSI, and encryption type per network
- Scan results chunked to respect 512 byte BLE notify limit
- WRITE_NR characteristic required on ESP32-C3 for reliable callback firing
- bleNotify() helper centralises TX notifications
- ntpSynced flag prevents time display before first sync
- Button long press placeholder -- bond clear active in v0.13.0
- Partition scheme changed to Minimal SPIFFS for larger app space (62% vs 93%)
- Full flash erase required on first flash and after BLE issues

### v0.11.1
- AP name and password configurable via WiFiManager portal
- AP settings saved to flash alongside timezone
- AP password validated -- must be 8+ chars (WPA2 requirement)

### v0.11
- Performance: strip.show() moved outside drawing loop
- Performance: loop() uses millis() -- time refreshes every 20s
- Non-blocking wait() with yield() replaces all delay() calls
- Timezone selectable via WiFiManager portal dropdown
- Timezone saved to flash with Preferences -- persists across reboots
- Long press button (pin 9) opens portal any time
- Status LED (pin 10) indicates connection state
- Font improvements: n, o, I redesigned for 5x5 legibility
- Serial output: IP address and RSSI logged after WiFi connect
- Colour constants use plain hex -- safe at global scope
- Variable shadowing fixed in DrawPixel()
- FadeString() uses const char* -- no heap fragmentation
- Off-by-one fixed in FadeString() loop

---

## Credits

- Nixie clock design inspiration: [PV Electronics](https://www.pvelectronics.co.uk/)
- Font based on [custom-fonts-for-microcontrollers](https://jared.geek.nz/2014/01/custom-fonts-for-microcontrollers) by Jared
- Board reference card by [@andypiper](https://github.com/andypiper/fivebyfive)
- Original sketch base from [01Space ESP32-C3FH4-RGB examples](https://github.com/01Space/ESP32-C3FH4-RGB)

---

## Roadmap

### v0.13.0
- BLE bonding security -- PIN displayed on matrix, bonded devices only
- Button 3s clears bonds and enters pairing mode
- Unknown devices rejected at BLE level

### Future Ideas
- MQTT message feed
- Outside temperature display
- Stock ticker
- Weather display
- Wordle game (see [wordle-device](https://github.com/ccattuto/wordle-device))
