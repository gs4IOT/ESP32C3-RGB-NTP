# ESP32C3 RGB NTP

**Version 0.12.1** — BLE configuration release

A WiFi-synced NTP clock running on the 01Space ESP32-C3FH4-RGB board with a 5×5 WS2812B NeoPixel matrix. Inspired by the PV Electronics single digit Nixie clock design based on source code of https://github.com/didn0t/5x5_Clock. Configured entirely over BLE — no USB or serial required after first flash.

---

## Hardware

| Component | Detail |
| --- | --- |
| Board | 01Space ESP32-C3FH4-RGB |
| Display | 25× WS2812B NeoPixel (5×5 matrix), pin 8 |
| Button | Built-in button, pin 9 |
| Status LED | Built-in blue LED, pin 10 |

---

## Features

- Syncs time via NTP on boot using saved WiFi credentials
- Displays time in amber every 20 seconds
- Displays date in blue on every minute change (DDMMYY format)
- Timezone configurable via BLE — saved to flash, survives reboots
- WiFi credentials sent over BLE — no portal or USB needed after first flash
- Long press button (3s) re-opens BLE pairing mode while clock is running
- Status LED lights up while connecting or BLE device is connected
- Non-blocking animations — no `delay()`, watchdog safe
- Reduced WiFi TX power (`WIFI_POWER_11dBm`) to lower current draw

---

## Radio Architecture

The ESP32-C3 has a **single shared radio** for both WiFi and BLE. They cannot run simultaneously. The firmware handles this by:

1. **Boot with no credentials** → BLE starts, waits for `ssid:` / `pass:` commands
2. **Boot with saved credentials** → WiFi connects, syncs NTP, WiFi off, BLE does not auto-start
3. **Long press button** → BLE starts manually after NTP is already synced
4. **NTP re-sync** → BLE advertising paused, WiFi connects, syncs, WiFi off

BLE uses `stopAdvertising()` before WiFi — not `deinit()` — to avoid a 60-second blocking delay.

---

## BLE Protocol

The firmware exposes a BLE UART-style service with two characteristics:

| Direction | UUID | Property |
| --- | --- | --- |
| Phone → Clock (RX) | `abcd1234-ab12-ab12-ab12-abcdef012345` | WRITE_NR |
| Clock → Phone (TX) | `abcd1235-ab12-ab12-ab12-abcdef012345` | READ + NOTIFY |

> **Note:** `PROPERTY_WRITE_NR` is required on ESP32-C3. Plain `WRITE` silently drops callbacks.

### Commands (phone → clock)

All commands must be **under 20 bytes** due to Android BLE WRITE_NR MTU limits.

| Command | Example | Description |
| --- | --- | --- |
| `ssid:` | `ssid:MyNetwork` | Buffer the WiFi SSID (send before `pass:`) |
| `pass:` | `pass:MyPassword` | Buffer password, save both to flash, reboot |
| `tz:` | `tz:America/Edmonton` | Change timezone, save to flash, apply immediately |
| `scan` | `scan` | Scan WiFi networks — returns SSID, RSSI, encryption |
| `time?` | `time?` | Return current local time string |

### Responses (clock → phone)

| Response | Meaning |
| --- | --- |
| `ssid_ok` | SSID buffered successfully |
| `wifi_saved -- rebooting` | Credentials saved, clock rebooting |
| `tz_ok` | Timezone applied and saved |
| `tz_err` | Invalid IANA timezone string |
| `networks:SSID(rssi,enc),...` | WiFi scan results |
| `err:send ssid: first` | `pass:` received without prior `ssid:` |
| `err:not synced yet` | `time?` received before NTP sync |
| `err:unknown cmd` | Unrecognised command |

> **Note:** `ssid:` and `pass:` are sent as separate commands to stay under the 20-byte BLE write limit.

---

## Required Libraries

Install via Arduino IDE → Tools → Manage Libraries:

| Library | Author |
| --- | --- |
| Adafruit NeoPixel | Adafruit |
| ezTime | Rop Gonggrijp |

`Preferences`, `WiFi`, and `BLE*` libraries are built into the ESP32 Arduino core — no install needed.

---

## File Structure

```
ESP32C3-RGB-NTP.ino   — main logic, BLE server, NTP sync, display loop
font.h                — 5×5 pixel font definitions
timezones.h           — IANA timezone reference list
```

---

## Board Settings (Arduino IDE)

| Setting | Value |
| --- | --- |
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |
| Flash Size | 4MB |

---

## First Upload

> **Full flash erase required before first upload.**
> Tools → Erase All Flash Before Sketch Upload → Enabled, upload once, then disable.

This clears any stale `Preferences` data that can cause BLE or WiFi issues.

---

## First Boot (no saved credentials)

1. Power on — matrix shows **"bLE"** then **"cfg"**
2. Open a BLE terminal app (e.g. Serial Bluetooth Terminal on Android)
3. Connect to **FingerClock**
4. Send `ssid:YourNetwork` (under 20 bytes)
5. Send `pass:YourPassword`
6. Clock replies `wifi_saved -- rebooting` and restarts
7. On reboot it connects to WiFi, syncs NTP, shows **"Up"**

---

## Changing WiFi or Timezone While Running

Hold the button on **pin 9 for 3 seconds**. The status LED turns solid blue and the matrix shows **"PAr"** — BLE starts and you can send new credentials or a `tz:` command.

- Sending new `ssid:` / `pass:` saves credentials and reboots
- Sending `tz:` applies immediately and saves to flash — no reboot needed

---

## Timing Constants

All timing constants are at the top of `ESP32C3-RGB-NTP.ino`:

| Constant | Default | Description |
| --- | --- | --- |
| `TIME_REFRESH_MS` | 20000 | How often time redraws (ms) |
| `FADE_STEP_MS` | 30 | Speed of fade animation per brightness step (ms) |
| `FADE_HOLD_MS` | 200 | How long character stays at full brightness (ms) |
| `LONG_PRESS_MS` | 3000 | Button hold time to open BLE (ms) |

---

## Colour Constants

| Constant | Hex | Used for |
| --- | --- | --- |
| `COL_RED` | `0xFF0000` | Errors, NTP prompt |
| `COL_GREEN` | `0x00FF00` | Success confirmations |
| `COL_BLUE` | `0x0000FF` | Date display, BLE prompt |
| `COL_AMBER` | `0x64C800` | Time display |

---

## Serial Monitor

Connect at **115200 baud** to see status output:

```
[+] FingerClock v0.12.1 booting...
[+] Pausing BLE before WiFi...
[+] Connecting to: MyNetwork
[+] WiFi connected -- syncing NTP
[+] UTC: Saturday, 21-Feb-2026 08:03:18 UTC
[+] TZ OK: America/Edmonton
[+] Local: Saturday, 21-Feb-2026 10:03:18 EET
[BLE] Phone connected
[BLE] Received: time?
[BLE] Phone disconnected
```

---

## Resetting Saved Settings

**Option A** — Full flash erase via Arduino IDE (see First Upload above).

**Option B** — Add temporarily to the top of `setup()`, flash once, then remove:

```cpp
prefs.begin("clock", false);
prefs.clear();
prefs.end();
```

---

## Known Limitations (v0.12.1)

- No BLE security — pairing is open, will be added in a future version
- WiFi scan (`scan` command) cannot run while BLE is connected — shared radio limitation
- Android BLE caps WRITE_NR at 20 bytes regardless of MTU negotiation — keep all commands short
- BLE does not auto-start after NTP sync — requires long press button

---

## Changelog

### v0.12.1
- BLE configuration replaces WiFiManager portal
- WiFi credentials sent via BLE `ssid:` / `pass:` commands — split to stay under 20-byte BLE limit
- Timezone configurable via BLE `tz:` command
- `scan` command returns WiFi networks with RSSI and encryption type
- `time?` command returns current local time string
- `PROPERTY_WRITE_NR` used on RX characteristic — required on ESP32-C3
- BLE and WiFi radio switching handled cleanly — `stopAdvertising()` used instead of `deinit()`
- Reduced WiFi TX power to `WIFI_POWER_11dBm`
- Status LED indicates BLE connection state
- Long press button restarts BLE while clock is running

### v0.11.1
- AP name and password configurable via WiFiManager portal
- AP settings saved to flash alongside timezone
- AP password validated — must be 8+ chars (WPA2 requirement)

### v0.11
- Performance: `strip.show()` moved outside drawing loop
- Non-blocking `wait()` with `yield()` replaces all `delay()` calls
- Timezone saved to flash with `Preferences`
- Font improvements: `n`, `o`, `I` redesigned for 5×5 legibility
- Variable shadowing fixed in `DrawPixel()`
- `FadeString()` uses `const char*` — no heap fragmentation
- Off-by-one fixed in `FadeString()` loop

---

## Future Ideas

- BLE security / bonding
- Battery operation with deep sleep
- Outside temperature display
- MQTT message feed
- Stock ticker
- Weather display
- Wordle game (see [wordle-device](https://github.com/ccattuto/wordle-device))

---

## Credits

- Nixie clock design inspiration: [PV Electronics](https://www.pvelectronics.co.uk/)
- Font based on [custom-fonts-for-microcontrollers](https://jared.geek.nz/2014/01/custom-fonts-for-microcontrollers) by Jared
- Board reference card by [@andypiper](https://github.com/andypiper/fivebyfive)
- Original sketch base from [01Space ESP32-C3FH4-RGB examples](https://github.com/01Space/ESP32-C3FH4-RGB)
