# ESP32C3 RGB NTP
**Version 0.11.1**

A WiFi-synced NTP clock based on a PV Electronics single digit Nixie clock design, running on the 01Space ESP32-C3FH4-RGB board with a 5×5 WS2812B NeoPixel matrix.

Displays time every 20 seconds and date in DDMMYY format every minute. Uses ezTime for NTP sync and a custom 5×5 pixel font based on [custom-fonts-for-microcontrollers](https://jared.geek.nz/2014/jan/custom-fonts-for-microcontrollers).

---

## Hardware

| Component | Detail |
|---|---|
| Board | 01Space ESP32-C3FH4-RGB |
| Display | 25× WS2812B NeoPixel (5×5 matrix), pin 8 |
| Button | Built-in button, pin 9 |
| Status LED | Built-in blue LED, pin 10 |

---

## Features

- Syncs time via NTP on boot
- Displays time in amber every 20 seconds
- Displays date in blue on every minute change
- Timezone selectable from portal dropdown — saved to flash, survives reboots
- Long press button (3s) opens WiFiManager portal to change WiFi or timezone
- Status LED lights up while connecting or portal is open
- Non-blocking animations — no delay(), watchdog safe

---

## Required Libraries

Install via Arduino IDE → Tools → Manage Libraries:

| Library | Author |
|---|---|
| Adafruit NeoPixel | Adafruit |
| ezTime | Rop Gonggrijp |
| WiFiManager | tzapu |

`Preferences` is built into the ESP32 Arduino core — no install needed.

---

## File Structure

```
NTC_Clock_LED.ino   — main logic
font.h              — 5×5 pixel font
timezones.h         — timezone dropdown list for portal
```

---

## Board Settings (Arduino IDE)

| Setting | Value |
|---|---|
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |
| Flash Size | 4MB |

---

## First Boot

1. Power on — matrix shows **"WiFi"**
2. Connect your phone/laptop to the `FingerClock` WiFi network
3. Password: `NTP323`
4. A portal page opens — select your WiFi network and timezone
5. Submit — matrix shows **"Up"** when connected and synced

---

## Changing WiFi or Timezone

Hold the button on **pin 9 for 3 seconds** while the clock is running. The status LED turns solid blue and the matrix shows **"rSt"** — the portal opens and you can pick new settings. Matrix shows **"ok"** when done.

---

## Resetting Saved Settings

To wipe all saved WiFi and timezone data:

**Option A** — Arduino IDE: Tools → Erase All Flash Before Sketch Upload → Enabled, then upload once, then disable again.

**Option B** — Add temporarily to the top of `setup()`, flash once, then remove:
```cpp
prefs.begin("clock", false);
prefs.clear();
prefs.end();
```

---

## Customisation

All timing constants are at the top of `NTC_Clock_LED.ino`:

| Constant | Default | Description |
|---|---|---|
| `TIME_REFRESH_MS` | 20000 | How often time redraws (ms) |
| `FADE_STEP_MS` | 30 | Speed of fade animation (ms) |
| `FADE_HOLD_MS` | 200 | How long character stays lit (ms) |
| `LONG_PRESS_MS` | 3000 | Button hold time for portal (ms) |

To add or remove timezones, edit `timezones.h`. Values must be valid IANA timezone strings — full list at [Wikipedia](https://en.wikipedia.org/wiki/List_of_tz_database_time_zones).

---

## Serial Monitor

Connect at **115200 baud** to see status output:

```
[+] Loaded timezone: America/Edmonton
[+] Connected to WiFi
[+] IP: 192.168.x.x
[+] RSSI: -68 dBm
[+] Syncing NTP...
[+] UTC: Saturday, 21-Feb-2026 08:03:18 UTC
[+] Timezone OK: America/Edmonton
[+] Local: Saturday, 21-Feb-2026 01:03:18 MST
```

---

## Changelog

### v0.11.1
- AP name and password configurable via WiFiManager portal
- AP settings saved to flash alongside timezone — persists across reboots
- AP password validated — must be 8+ chars (WPA2 requirement)
- autoConnect() now uses saved AP credentials instead of hardcoded values

### v0.11
- Performance: `strip.show()` moved outside drawing loop
- Performance: `loop()` uses `millis()` — time refreshes every 20s
- Non-blocking `wait()` with `yield()` replaces all `delay()` calls
- Timezone selectable via WiFiManager portal dropdown
- Timezone saved to flash with `Preferences` — persists across reboots
- Long press button (pin 9) opens portal any time
- Status LED (pin 10) indicates connection state
- Timezone list extracted to `timezones.h`
- Font improvements: `n`, `o`, `I` redesigned for 5×5 legibility
- Serial output: IP address and RSSI logged after WiFi connect
- Colour constants use plain hex — safe at global scope
- Variable shadowing fixed in `DrawPixel()`
- `FadeString()` uses `const char*` — no heap fragmentation
- Off-by-one fixed in `FadeString()` loop

---

## Credits

- Nixie clock design inspiration: [PV Electronics](https://www.pvelectronics.co.uk/)
- Font based on [custom-fonts-for-microcontrollers](https://jared.geek.nz/2014/01/custom-fonts-for-microcontrollers) by Jared
- Board reference card by [@andypiper](https://github.com/andypiper/fivebyfive)
- Original sketch base from [01Space ESP32-C3FH4-RGB examples](https://github.com/01Space/ESP32-C3FH4-RGB)

---

## Future Ideas

- MQTT message feed
- Outside temperature display
- Stock ticker
- BLE control from phone
- Weather display
- Wordle game (see [wordle-device](https://github.com/ccattuto/wordle-device))
