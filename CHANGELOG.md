# Changelog

All notable changes to the ESP32-C3 BLE NTP clock project.

---

## v0.12.6 — fixes, date? command, rbt restored

### Added

- **`date?` BLE command** — returns current date and timezone string (e.g. `23/02/2026 MST`), separate from `time?` to stay safely under the 20-byte BLE limit

### Fixed

- **`pass:` radio conflict** — was incorrectly calling `syncNTP()` directly from BLE callback while BLE was still active; WiFi and BLE share the same radio and cannot run simultaneously — now saves credentials, shows `rbt`, and reboots; WiFi connects cleanly on reboot
- **`tz:` radio conflict** — was attempting `myTZ.setLocation()` while BLE was active, which requires WiFi; now always saves to flash, shows `rbt`, and reboots to apply
- **`time?` format** — was hardcoded to 24h regardless of `hour12` setting; now uses `timeFormat()` to match the display — returns `"14:43"` or `"2:43 PM"` correctly
- **`time?` 20-byte limit** — old format `"H:i:s d/m/Y T"` with full date could exceed 20 bytes; now returns time only, date available separately via `date?`

### Restored

- **`"rbt"` matrix message** — shown in green before every `ESP.restart()` (`pass:`, `tz:`, `reboot:`) — was present in v0.12.1 and lost in v0.12.2

### Removed

- **Dead `"nO"` guard in `syncNTP()`** — `setup()` already handles the empty credentials case before calling `syncNTP()`, making that path unreachable

---

## v0.12.5 — WiFi retry, chase + radar animations

### Added

- **WiFi retry loop** — 3 attempts max, 10s timeout per attempt
  - Shows attempt number (1/2/3) in red before each try
  - Chase animation runs during each attempt
  - Attempt colours: 1 = blue, 2 = amber, 3 = red
  - 2s pause between failed attempts
- **Auto BLE config** after 3 WiFi failures — no manual button press needed
- **`chaseAnimation(colour)`** — three pixels spaced evenly chasing across the matrix, `millis()`-driven, non-blocking
- **`radarAnimation()`** — rings expanding from centre (ring 0 = centre, ring 1 = inner 8px, ring 2 = outer 16px, ring 3 = blank pause), green, 200ms per frame
- **Periodic resync failure** auto-enters BLE config

### Changed

- **Radar animation** only runs when `!ntpSynced` — avoids conflict with time display when BLE is active and clock is synced; status LED on pin 10 indicates BLE state instead
- **`strip.clear()`** called in `onConnect()` to cleanly end radar when phone connects
- **`strip.clear()`** called on both BLE timeout paths to cleanly end radar

---

## v0.12.4 — info?, resync:N, periodic NTP re-sync

### Added

- **`info?` BLE command** — returns version, SSID, timezone, brightness, format, flip, resync interval, uptime, and last sync time as multiple notifies with 50ms gap
- **`resync:N` BLE command** — set NTP re-sync interval in hours (1–168, default 100), saved to flash
- **Periodic NTP re-sync** — configurable interval, reconnects WiFi briefly, skips if BLE is active (shared radio guard)
- **`lastSync` / `lastSyncUnix`** tracking — records millis() and UTC unix time after every successful sync
- **`bleNotifyLine()` helper** — notify with 50ms pause to avoid dropped packets on back-to-back sends
- **`uptimeString()` helper** — returns uptime as "Xd Yh Zm" from `millis()`

---

## v0.12.3 — brightness, 12/24h, display flip, project rename

### Added

- **`bright:N` BLE command** — set brightness level 1–10 (level × 15 = actual, range 15–150), applied immediately, saved to flash
- **`clock12:` / `clock24:` BLE commands** — switch 12h/24h time format, saved to flash
  - 12h format string: `"g:i A"` — uppercase AM/PM for better readability on 5×5 matrix
- **`flip:on` / `flip:off` BLE commands** — rotate display 180° for USB-top mounting, saved to flash
- **`timeFormat()` helper** — returns format string based on 12/24h setting, single point of change
- **Boot settings dump** — brightness, time format, display orientation printed to Serial on startup
- **`bleActive` flag** commented on every `deinit()` path to document the silent-return bug

### Fixed

- **`h24`/`hour12` inversion bug** — save and load the same bool directly with no inversion

### Changed

- Project renamed from `ESP32C3-RGB-NTP` to `ESP32-C3-BLE-NTP`

---

## v0.12.2 — reboot: command, time? Serial output

### Added

- **`reboot:` BLE command** — remote reboot with 500ms delay before `ESP.restart()`
- **`time?`** now also prints current time to Serial monitor
- **Unknown command response** now lists all valid commands

### Fixed

- **`reboot:` inside BLE callback** — `BLEDevice::deinit(true)` blocks indefinitely when called from a callback; removed deinit, simple `wait(500)` + `ESP.restart()` works correctly

---

## v0.12.1 — BLE configuration

### Added

- BLE configuration replaces WiFiManager portal
- WiFi credentials sent via `ssid:` / `pass:` — split to stay under 20-byte BLE WRITE_NR limit
- Credentials saved to flash, NTP syncs immediately after saving
- Timezone via `tz:` command — saved to flash, applied on next reboot
- `time?` command returns current local time string
- `PROPERTY_WRITE_NR` used on RX characteristic — required on ESP32-C3 (plain `WRITE` silently drops callbacks)
- BLE and WiFi never run simultaneously — clean radio sequencing
- WiFi TX power set to `WIFI_POWER_11dBm` — prevents brownout on cold boot
- BLE auto-timeout: 3 min no connect, 1 min after disconnect
- Status LED indicates BLE connection state
- Long press button (3s) starts BLE while clock is running
- Both radios idle after NTP sync — BLE only on demand

### Removed

- `scan` command — cannot run WiFi scan while BLE stack is active

---

## v0.11.1 — WiFiManager portal improvements

### Added

- AP name and password configurable via WiFiManager portal
- AP settings saved to flash alongside timezone
- AP password validated — must be 8+ chars (WPA2 requirement)

---

## v0.11 — performance and stability

### Changed

- `strip.show()` moved outside drawing loop — called once per character, not per pixel
- Non-blocking `wait()` with `yield()` replaces all `delay()` calls
- Timezone saved to flash with `Preferences`
- `FadeString()` uses `const char*` — no heap fragmentation

### Fixed

- Variable shadowing in `DrawPixel()`
- Font improvements: `n`, `o`, `I` redesigned for 5×5 legibility
