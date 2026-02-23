# ESP32C3 RGB NTP

**Version 0.12.1** — BLE configuration release

A WiFi-synced NTP clock running on the 01Space ESP32-C3FH4-RGB board with a 5x5 WS2812B NeoPixel matrix. Inspired by the PV Electronics single digit Nixie clock design based on source code of https://github.com/didn0t/5x5_Clock. Configured entirely over BLE — no USB or serial required after first flash.

---

## Hardware

| Component | Detail |
| --- | --- |
| Board | 01Space ESP32-C3FH4-RGB |
| Display | 25x WS2812B NeoPixel (5x5 matrix), pin 8 |
| Button | Built-in button, pin 9 |
| Status LED | Built-in blue LED, pin 10 |

---

## Features

- Syncs time via NTP on boot using saved WiFi credentials
- Displays time in amber every 20 seconds
- Displays date in blue on every minute change (DDMMYY format)
- Timezone configurable via BLE `tz:` command — saved to flash, applies on next reboot
- WiFi credentials sent over BLE — no portal or USB needed after first flash
- Long press button (3s) opens BLE while clock is running
- Status LED lights while connecting or BLE device is connected
- Non-blocking animations — no `delay()`, watchdog safe
- Reduced WiFi TX power (`WIFI_POWER_11dBm`) to prevent brownout on cold boot
- BLE auto-timeout: 3 minutes with no connection, 1 minute after disconnect

---

## Radio Architecture

The ESP32-C3 has a **single shared radio** for both WiFi and BLE. They cannot run simultaneously. The firmware handles this by:

1. **First boot (no credentials)** — BLE starts, waits for `ssid:` / `pass:` commands, reboots after saving
2. **Normal boot (credentials saved)** — WiFi connects, syncs NTP, WiFi off, both radios idle
3. **Long press button** — BLE starts manually, auto-shuts off after timeout
4. **Timezone change** — saved to flash via BLE, applied on next reboot when WiFi is active

Key lessons learned:
- `BLEDevice::deinit()` blocks for up to 60 seconds — use `stopAdvertising()` for short pauses
- `myTZ.setLocation()` requires active WiFi — cannot apply timezone while BLE is running
- WiFi TX power must be set inside `syncNTP()` before `WiFi.begin()` — not in `setup()`
- Android BLE WRITE_NR caps at 20 bytes regardless of MTU negotiation
- `PROPERTY_WRITE_NR` required on ESP32-C3 — plain `WRITE` silently drops callbacks

---

## BLE Protocol

The firmware exposes a BLE UART-style service with two characteristics:

| Direction | UUID | Property |
| --- | --- | --- |
| Phone to Clock (RX) | `abcd1234-ab12-ab12-ab12-abcdef012345` | WRITE_NR |
| Clock to Phone (TX) | `abcd1235-ab12-ab12-ab12-abcdef012345` | READ + NOTIFY |

### Commands (phone to clock)

All commands must be **under 20 bytes** due to Android BLE WRITE_NR limit.

| Command | Example | Description |
| --- | --- | --- |
| `ssid:` | `ssid:MyNetwork` | Buffer the WiFi SSID (send before `pass:`) |
| `pass:` | `pass:MyPassword` | Save credentials to flash, reboot |
| `tz:` | `tz:America/Edmonton` | Save timezone to flash, applies on next reboot |
| `time?` | `time?` | Return current local time string |

### Responses (clock to phone)

| Response | Meaning |
| --- | --- |
| `ssid_ok -- now send pass:` | SSID buffered successfully |
| `wifi_saved -- rebooting` | Credentials saved, clock rebooting |
| `tz_saved -- reboot to apply` | Timezone saved, will apply on next reboot |
| `err:send ssid: first` | `pass:` received without prior `ssid:` |
| `err:no time sync yet` | `time?` received before NTP sync |
| `err:unknown` | Unrecognised command |

> **Note:** `ssid:` and `pass:` are sent as separate commands to stay under the 20-byte BLE write limit. Non-ASCII characters in SSID (e.g. Cyrillic) use 2 bytes each and will truncate — use ASCII-only network names.

---

## BLE Auto-Timeout

BLE shuts off automatically to keep radios idle during normal clock operation:

| Condition | Timeout |
| --- | --- |
| No phone connects after button press | 3 minutes |
| Phone disconnects | 1 minute |

After timeout both radios are off. Press button 3s to re-enable BLE.

---

## Required Libraries

Install via Arduino IDE — Tools — Manage Libraries:

| Library | Author |
| --- | --- |
| Adafruit NeoPixel | Adafruit |
| ezTime | Rop Gonggrijp |

`Preferences`, `WiFi`, and `BLE*` are built into the ESP32 Arduino core — no install needed.

---

## File Structure

```
ESP32C3-RGB-BLE-v0.12.1.ino   -- main logic, BLE server, NTP sync, display loop
ESP32C3-RGB-NTP.ino            -- v0.11.1 WiFiManager version (kept for reference)
font.h                         -- 5x5 pixel font definitions
timezones.h                    -- IANA timezone reference list (v0.11.1 only)
```

---

## Board Settings (Arduino IDE)

| Setting | Value |
| --- | --- |
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| CPU Frequency | 160MHz (WiFi) |
| Flash Size | 4MB |
| Partition Scheme | Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS) |
| Upload Speed | 921600 |
| Erase All Flash | Disabled (enable only for first upload) |

> **Partition scheme is critical** — default partition is too small for BLE + WiFi together.

---

## First Upload

> **Full flash erase required before first upload.**
> Tools — Erase All Flash Before Sketch Upload — Enabled, upload once, then disable.

This clears stale `Preferences` data that can cause BLE or WiFi issues on first boot.

---

## First Boot (no saved credentials)

1. Power on — matrix shows **"bLE"** then **"cfg"**
2. Open a BLE terminal app (e.g. LightBlue or nRF Connect)
3. Connect to **FingerClock**
4. Send `ssid:YourNetwork` (under 20 bytes, ASCII only)
5. Send `pass:YourPassword`
6. Clock replies `wifi_saved -- rebooting` and restarts
7. On reboot — WiFi connects, NTP syncs, matrix shows **"Up"**

---

## Normal Boot (credentials saved)

1. Power on — matrix shows **"ntp"**
2. WiFi connects and NTP syncs
3. Matrix shows **"Up"** then time display begins
4. Both radios go idle — no WiFi, no BLE

---

## Changing WiFi or Timezone While Running

Hold the button on **pin 9 for 3 seconds**. Matrix shows **"bLE"**, status LED turns on, BLE starts advertising.

- Send new `ssid:` + `pass:` — saves and reboots
- Send `tz:America/Vancouver` — saves to flash, reboot to apply
- BLE shuts off after 3 minutes idle or 1 minute after disconnect

---

## Timing Constants

| Constant | Default | Description |
| --- | --- | --- |
| `TIME_REFRESH_MS` | 20000 | How often time redraws (ms) |
| `FADE_STEP_MS` | 30 | Speed of fade animation per step (ms) |
| `FADE_HOLD_MS` | 200 | How long character stays at full brightness (ms) |
| `LONG_PRESS_MS` | 3000 | Button hold time to open BLE (ms) |
| `BLE_ADVERTISE_TIMEOUT_MS` | 180000 | BLE off if no connection (ms) |
| `BLE_DISCONNECT_TIMEOUT_MS` | 60000 | BLE off after disconnect (ms) |

---

## Colour Constants

| Constant | Hex | Used for |
| --- | --- | --- |
| `COL_RED` | `0xFF0000` | Errors, NTP prompt |
| `COL_GREEN` | `0x00FF00` | Success confirmations |
| `COL_BLUE` | `0x0000FF` | Date display, BLE prompt |
| `COL_AMBER` | `0x64C800` | Time display |

---

## Matrix Messages

| Message | Colour | Meaning |
| --- | --- | --- |
| `ntp` | Red | Connecting to WiFi and syncing time |
| `Up` | Green | Boot successful, time synced |
| `Err` | Red | WiFi or NTP failed |
| `bLE` | Blue | BLE starting |
| `cfg` | Amber | Waiting for BLE credentials |
| `rbt` | Green | Rebooting after credentials saved |
| `ok` | Green | Command accepted |

---

## Serial Monitor

Connect at **115200 baud** to see status output:

```
[+] FingerClock v0.12.1 booting...
[+] Connecting to: MyNetwork
[+] WiFi connected -- syncing NTP
[+] UTC: Sunday, 22-Feb-2026 22:43:37 UTC
[+] TZ OK: America/Vancouver
[+] Local: Sunday, 22-Feb-2026 14:43:37 PST
[+] WiFi off -- hold button to start BLE
[+] Button held...
[!] Long press -- restarting BLE
[BLE] Starting...
[BLE] Advertising -- connect with nRF Connect or LightBlue
[BLE] Phone connected -- MTU: 512
[BLE] Received: tz:America/Edmonton
[BLE] TZ saved (applies on reboot): America/Edmonton
[BLE] Phone disconnected -- BLE off in 1 min
[BLE] Post-disconnect timeout -- shutting off
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

- No BLE security — pairing is open, planned for a future version
- Android BLE caps WRITE_NR at 20 bytes regardless of MTU — keep all commands short
- Non-ASCII SSID names (Cyrillic etc.) truncate at 20 bytes — use ASCII network names
- Timezone changes require reboot to apply — `myTZ.setLocation()` needs active WiFi
- First cold boot may brownout reset once before succeeding — second boot always clean

---

## Changelog

### v0.12.1
- BLE configuration replaces WiFiManager portal
- WiFi credentials sent via `ssid:` / `pass:` — split to stay under 20-byte BLE limit
- Credentials saved to flash, board reboots after saving — prevents radio conflict on first config
- Timezone via `tz:` command — saved to flash, applied on next reboot
- `time?` command returns current local time string
- `PROPERTY_WRITE_NR` used on RX characteristic — required on ESP32-C3
- BLE and WiFi never run simultaneously — clean radio sequencing
- WiFi TX power set to `WIFI_POWER_11dBm` — prevents brownout on cold boot
- BLE auto-timeout: 3 min no connect, 1 min after disconnect
- Status LED indicates BLE connection state
- Long press button (3s) starts BLE while clock is running
- Both radios idle after NTP sync — BLE only on demand
- `scan` command removed — cannot run WiFi scan while BLE stack is active

### v0.11.1
- AP name and password configurable via WiFiManager portal
- AP settings saved to flash alongside timezone
- AP password validated — must be 8+ chars (WPA2 requirement)

### v0.11
- Performance: `strip.show()` moved outside drawing loop
- Non-blocking `wait()` with `yield()` replaces all `delay()` calls
- Timezone saved to flash with `Preferences`
- Font improvements: `n`, `o`, `I` redesigned for 5x5 legibility
- Variable shadowing fixed in `DrawPixel()`
- `FadeString()` uses `const char*` — no heap fragmentation

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
