# ESP32C3 RGB NTP

**Version 0.12.6** — fixes, date? command, rbt restored

A WiFi-synced NTP clock running on the 01Space ESP32-C3FH4-RGB board with a 5×5 WS2812B NeoPixel matrix. Inspired by the PV Electronics single digit Nixie clock design based on source code of <https://github.com/didn0t/5x5_Clock>. Configured entirely over BLE — no USB or serial required after first flash.

---

## Hardware

| Component | Detail |
| --- | --- |
| Board | 01Space ESP32-C3FH4-RGB |
| Display | 25× WS2812B NeoPixel (5×5 matrix), pin 8 |
| Button | Built-in button, pin 9 (INPUT_PULLUP, hold 3s for BLE) |
| Status LED | Built-in blue LED, pin 10 |

---

## Features

* Syncs time via NTP on boot using saved WiFi credentials
* WiFi retry with visual feedback — 3 attempts, chase animation per attempt
* Auto BLE config mode after 3 WiFi failures — no manual button press needed
* Displays time in amber every 20 seconds
* Displays date in blue on every minute change (DDMMYY format)
* 12h / 24h time format via BLE — saved to flash
* Adjustable brightness (10 levels) via BLE — saved to flash
* Display flip (180°) via BLE — saved to flash
* Configurable NTP re-sync interval (1–168 hours) via BLE — saved to flash
* Periodic NTP re-sync — skips if BLE is active (shared radio guard)
* Periodic resync failure auto-enters BLE config
* Timezone configurable via BLE `tz:` command — saved to flash, applied on reboot
* WiFi credentials sent over BLE — saved to flash, clock reboots to connect
* Long press button (3s) opens BLE while clock is running
* Chase animation during WiFi connect — colour changes per attempt
* Radar animation during BLE advertising (when clock not synced)
* Device info query via BLE — version, settings, uptime, last sync
* Current time via BLE `time?` — respects 12h/24h setting
* Current date via BLE `date?` — date and timezone string
* Remote reboot via BLE
* Status LED lights while connecting or BLE device is connected
* Non-blocking animations — no `delay()`, watchdog safe
* Reduced WiFi TX power (`WIFI_POWER_11dBm`) to prevent brownout on cold boot
* BLE auto-timeout: 3 minutes with no connection, 1 minute after disconnect

---

## Radio Architecture

The ESP32-C3 has a **single shared radio** for both WiFi and BLE. They cannot run simultaneously. The firmware handles this by:

1. **First boot (no credentials)** — BLE starts, waits for `ssid:` / `pass:` commands, saves to flash, shows `rbt`, reboots — WiFi connects on reboot
2. **Normal boot (credentials saved)** — WiFi connects (up to 3 attempts), syncs NTP, WiFi off, both radios idle
3. **WiFi failure (all 3 attempts)** — auto-enters BLE config mode, no button press required
4. **Long press button** — BLE starts manually, auto-shuts off after timeout
5. **Periodic re-sync** — WiFi on briefly to re-sync NTP, skips if BLE is active
6. **Periodic resync failure** — auto-enters BLE config

---

## WiFi Retry

The firmware attempts WiFi connection up to 3 times before falling back to BLE config:

| Attempt | Colour | Timeout |
| --- | --- | --- |
| 1 | Blue | 10 seconds |
| 2 | Amber | 10 seconds |
| 3 | Red | 10 seconds |

Each attempt shows its number (1/2/3) in red before connecting, then runs a chase animation in the attempt colour while waiting. A 2-second pause separates failed attempts. After 3 failures the clock enters BLE config automatically.

---

## Animations

| Animation | Trigger | Description |
| --- | --- | --- |
| Chase | WiFi connecting | Three pixels spaced evenly, chasing across the matrix. Colour matches attempt number. |
| Radar | BLE advertising (clock not synced) | Rings expanding from centre — centre → inner 8px → outer 16px → blank pause. Green, 200ms per frame. |

Both animations are `millis()`-driven and non-blocking — `yield()` still feeds the WiFi/BLE stack. Radar only runs when `!ntpSynced` to avoid conflict with time display. When BLE is active and clock is synced, the status LED on pin 10 indicates BLE state instead.

---

## BLE Protocol

The firmware exposes a BLE UART-style service with two characteristics:

| Direction | UUID | Property |
| --- | --- | --- |
| Phone to Clock (RX) | `abcd1234-ab12-ab12-ab12-abcdef012345` | WRITE_NR |
| Clock to Phone (TX) | `abcd1235-ab12-ab12-ab12-abcdef012345` | READ + NOTIFY |

Service UUID: `12345678-1234-1234-1234-123456789abc`

### Commands (phone to clock)

All commands must be **under 20 bytes** due to Android BLE WRITE_NR limit.

| Command | Example | Description |
| --- | --- | --- |
| `ssid:` | `ssid:MyNetwork` | Buffer the WiFi SSID (send before `pass:`) |
| `pass:` | `pass:MyPassword` | Save credentials to flash, show `rbt`, reboot — WiFi connects on reboot |
| `tz:` | `tz:America/Edmonton` | Save timezone to flash, show `rbt`, reboot — applied on next boot |
| `time?` | `time?` | Return current local time — respects 12h/24h setting |
| `date?` | `date?` | Return current date and timezone string |
| `info?` | `info?` | Return version, SSID, timezone, brightness, format, flip, resync, uptime, last sync |
| `reboot:` | `reboot:` | Show `rbt`, reboot the device |
| `bright:N` | `bright:5` | Set brightness level 1–10 (level × 15 = actual, range 15–150) |
| `clock12:` | `clock12:` | Switch to 12-hour format |
| `clock24:` | `clock24:` | Switch to 24-hour format (default) |
| `flip:on` | `flip:on` | Flip display 180° (USB top) |
| `flip:off` | `flip:off` | Normal orientation (USB bottom, default) |
| `resync:N` | `resync:24` | Set NTP re-sync interval in hours (1–168, default 100) |

### Responses (clock to phone)

| Response | Meaning |
| --- | --- |
| `ssid_ok -- now send pass:` | SSID buffered successfully |
| `wifi_saved -- rebooting...` | Credentials saved, clock rebooting |
| `tz_saved -- rebooting...` | Timezone saved, clock rebooting to apply |
| `bright_ok` | Brightness level saved |
| `clock12_ok` / `clock24_ok` | Time format saved |
| `flip_ok` | Display orientation saved |
| `resync_ok:Nh` | Re-sync interval saved |
| `rebooting...` | Device rebooting in 500ms (`reboot:` command) |
| `ver:0.12.6` | First line of `info?` response |
| `---` | End of `info?` response |
| `err:send ssid: first` | `pass:` received without prior `ssid:` |
| `err:no sync yet` | `time?` or `date?` received before NTP sync |
| `err:bright must be 1-10` | Brightness out of range |
| `err:resync must be 1-168` | Resync interval out of range |
| `err:unknown -- valid: ...` | Unrecognised command (lists all valid commands) |

> **Note:** `ssid:` and `pass:` are sent as separate commands to stay under the 20-byte BLE write limit. Non-ASCII characters in SSID (e.g. Cyrillic) use 2 bytes each and will truncate — use ASCII-only network names.

> **Note:** `info?` sends multiple short notifies with a 50ms gap between each to avoid dropped packets on Android.

---

## BLE Auto-Timeout

BLE shuts off automatically to keep radios idle during normal clock operation:

| Condition | Timeout |
| --- | --- |
| No phone connects after button press | 3 minutes |
| Phone disconnects | 1 minute |

After timeout both radios are off. Press button 3s to re-enable BLE.

---

## Preferences (flash storage)

All settings are stored in the `"clock"` namespace using the ESP32 Preferences library:

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `ssid` | String | — | WiFi network name |
| `pass` | String | — | WiFi password |
| `tz` | String | `America/Edmonton` | IANA timezone string |
| `bright` | uint8 | 5 | Brightness level 1–10 (level × 15 = actual value, range 15–150) |
| `hour12` | bool | false | true = 12h, false = 24h |
| `flip` | bool | false | true = USB top, false = USB bottom |
| `resync` | uint8 | 100 | NTP re-sync interval in hours (1–168) |

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
ESP32-C3-BLE-NTP-v0_12_6.ino   -- current release
ESP32-C3-BLE-NTP-v0_12_5.ino   -- previous versions (kept for reference)
ESP32-C3-BLE-NTP-v0_12_4.ino
ESP32-C3-BLE-NTP-v0_12_3.ino
ESP32-C3-BLE-NTP-v0_12_2.ino
ESP32-C3-BLE-NTP-v0_12_1.ino
ESP32C3-RGB-NTP.ino             -- v0.11.1 WiFiManager version (kept for reference)
font.h                          -- 5x5 pixel font definitions
timezones.h                     -- IANA timezone reference list (v0.11.1 only)
CHANGELOG.md                    -- full version history
RELEASE_NOTES_v0_12_6.md        -- release notes for current version
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

> **Partition scheme is critical** — default partition leaves only 6% headroom (94% used). Minimal SPIFFS gives 38% headroom (62% used). The BLE stack is the large fixed cost — application logic on top is nearly flat per version.

---

## First Upload

> **Full flash erase required before first upload.**
> Tools → Erase All Flash Before Sketch Upload → Enabled, upload once, then disable.

This clears stale `Preferences` data that can cause BLE or WiFi issues on first boot.

---

## First Boot (no saved credentials)

1. Power on — matrix shows **"bLE"** then **"cfg"**
2. Open a BLE terminal app (e.g. LightBlue or nRF Connect)
3. Connect to **FingerClock**
4. Send `ssid:YourNetwork` (under 20 bytes, ASCII only)
5. Send `pass:YourPassword`
6. Clock syncs NTP immediately — matrix shows **"ntp"** then **"ok"** on success
7. Time display begins

---

## Normal Boot (credentials saved)

1. Power on — matrix shows **"ntp"**
2. WiFi connects (up to 3 attempts with chase animation)
3. NTP syncs — matrix shows **"Up"**
4. Time display begins, both radios go idle

If all 3 WiFi attempts fail, the clock automatically enters BLE config mode — no button press needed.

---

## Changing Settings While Running

Hold the button on **pin 9 for 3 seconds**. Matrix shows **"bLE"**, status LED turns on, BLE starts advertising.

* Send new `ssid:` + `pass:` — syncs NTP immediately
* Send `tz:America/Vancouver` — applies immediately if possible, otherwise reboot to apply
* Send `bright:8` — adjusts brightness instantly
* Send `clock12:` or `clock24:` — switches time format
* Send `flip:on` or `flip:off` — rotates display
* Send `resync:24` — changes re-sync interval
* Send `info?` — get current device state
* Send `reboot:` — restart the device
* BLE shuts off after 3 minutes idle or 1 minute after disconnect

---

## Timing Constants

| Constant | Default | Description |
| --- | --- | --- |
| `TIME_REFRESH_MS` | 20000 | How often time redraws (ms) |
| `FADE_STEP_MS` | 30 | Speed of fade animation per step (ms) |
| `FADE_HOLD_MS` | 200 | How long character stays at full brightness (ms) |
| `LONG_PRESS_MS` | 3000 | Button hold time to open BLE (ms) |
| `WIFI_ATTEMPT_MS` | 10000 | Timeout per WiFi attempt (ms) |
| `WIFI_MAX_ATTEMPTS` | 3 | Total WiFi attempts before BLE fallback |
| `CHASE_STEP_MS` | 50 | Chase animation frame time (ms) |
| `RADAR_STEP_MS` | 200 | Radar animation frame time (ms) |
| `BLE_ADVERTISE_TIMEOUT_MS` | 180000 | BLE off if no connection (ms) |
| `BLE_DISCONNECT_TIMEOUT_MS` | 60000 | BLE off after disconnect (ms) |

---

## Colour Constants

| Constant | Hex | Used for |
| --- | --- | --- |
| `COL_RED` | `0xFF0000` | Errors, NTP prompt, WiFi attempt 3 |
| `COL_GREEN` | `0x00FF00` | Success confirmations, radar animation |
| `COL_BLUE` | `0x0000FF` | Date display, BLE prompt, WiFi attempt 1 |
| `COL_AMBER` | `0x64C800` | Time display, WiFi attempt 2 |

---

## Matrix Messages

| Message | Colour | Meaning |
| --- | --- | --- |
| `ntp` | Red | Connecting to WiFi and syncing time |
| `Up` | Green | Boot successful, time synced |
| `Err` | Red | WiFi or NTP failed |
| `bLE` | Blue | BLE starting |
| `cfg` | Amber | Waiting for BLE credentials |
| `ok` | Green | Command accepted |
| `rbt` | Green | Rebooting — shown before every `ESP.restart()` |
| `1` `2` `3` | Red | WiFi attempt number |

---

## Serial Monitor

Connect at **115200 baud** to see status output:

```
[+] FingerClock v0.12.6 booting...
[+] Brightness: level 5 (75/150)
[+] Time format: 24h
[+] Display: normal (USB bottom)
[+] Resync interval: 100h
[+] WiFi attempt 1 of 3: MyNetwork
[+] WiFi connected -- syncing NTP
[+] UTC: Sunday, 22-Feb-2026 22:43:37 UTC
[+] TZ OK: America/Edmonton
[+] Local: Sunday, 22-Feb-2026 15:43:37 MST
[+] WiFi off -- hold button to start BLE
[+] Button held...
[!] Long press -- restarting BLE
[BLE] Starting...
[BLE] Advertising -- connect with nRF Connect or LightBlue
[BLE] Phone connected -- MTU: 512
[BLE] Received: time?
[BLE] time? -> 15:43
[BLE] Received: date?
[BLE] date? -> 22/02/2026 MST
[BLE] Received: tz:America/Vancouver
[BLE] TZ saved: America/Vancouver -- rebooting to apply
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

## Hard-Won Lessons

* `PROPERTY_WRITE_NR` required on ESP32-C3 — plain `WRITE` silently drops callbacks
* `bleActive` **must** be reset to `false` after every `deinit()` — if left `true`, `startBLE()` silently returns
* `ESP.restart()` works fine inside BLE callback — just needs `wait()` before it
* `BLEDevice::deinit(true)` called inside BLE callback blocks indefinitely — never call from callback
* `syncNTP()` cannot be called from a BLE callback — WiFi and BLE share the same radio — save credentials and reboot instead
* `myTZ.setLocation()` requires active WiFi — save timezone and reboot, never call while BLE is running
* `WiFi.setTxPower(WIFI_POWER_11dBm)` must be called inside `syncNTP()` before `WiFi.begin()`
* `WIFI_POWER_11dBm` prevents brownout on cold boot
* Animation functions writing to strip in `loop()` conflict with `FadeString()` — guard with `!ntpSynced`
* `millis()`-driven animations are safe — no blocking, `yield()` still feeds WiFi/BLE stack
* `h24`/`hour12` inversion bug — save and load the same bool directly, no inversion
* 12h format string: `"g:i A"` — uppercase AM/PM reads better on 5×5 matrix
* `time?` uses `timeFormat()` — returns `"g:i A"` or `"H:i"` matching the display
* Multiple back-to-back BLE notifies need 50ms gap or packets drop
* Non-ASCII SSIDs use 2 bytes per character — truncate silently at 20 bytes
* Full flash erase required before first upload to clear stale Preferences data
* `strip.clear()` called in `onConnect()` to cleanly end radar when phone connects
* `strip.clear()` called on both BLE timeout paths to cleanly end radar

---

## Known Limitations (v0.12.6)

* No BLE security — pairing is open, planned for v0.13.0
* Android BLE caps WRITE_NR at 20 bytes regardless of MTU — keep all commands short
* Non-ASCII SSID names (Cyrillic etc.) truncate at 20 bytes — use ASCII network names
* Timezone changes always require reboot — `myTZ.setLocation()` needs active WiFi
* First cold boot may brownout reset once before succeeding — second boot always clean
* Radar animation pixel indices assume USB-bottom orientation — `flip` setting does not rotate animations

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for full version history.

---

## Future Ideas

* **v0.13.0** — BLE security / bonding with PIN display on matrix
* Deep sleep mode (needs use case decision — always-on vs battery)
* Outside temperature display
* MQTT message feed
* Stock ticker
* Weather display
* Wordle game (see [wordle-device](https://github.com/ccattuto/wordle-device))

---

## Credits

* Nixie clock design inspiration: [PV Electronics](https://www.pvelectronics.co.uk/)
* Font based on [custom-fonts-for-microcontrollers](https://jared.geek.nz/2014/01/custom-fonts-for-microcontrollers) by Jared
* Board reference card by [@andypiper](https://github.com/andypiper/fivebyfive)
* Original sketch base from [01Space ESP32-C3FH4-RGB examples](https://github.com/01Space/ESP32-C3FH4-RGB)
