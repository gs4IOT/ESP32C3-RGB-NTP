# Release Notes — v0.12.6

**ESP32-C3 BLE NTP Clock**
Released: February 2026

---

## Summary

v0.12.6 is a patch release. It fixes two radio sequencing bugs that crept in during v0.12.2 development, restores the `"rbt"` matrix message that was lost at the same time, splits `time?` into `time?` + `date?` to respect the 20-byte BLE limit, and fixes `time?` to respect the `clock12:`/`clock24:` setting.

No new features. No hardware changes. Flash and go.

---

## What Changed

### Two radio sequencing bugs fixed

The ESP32-C3 has a single shared radio. BLE and WiFi cannot run simultaneously. This is documented throughout the codebase — but two commands were violating it silently.

**`pass:` was calling `syncNTP()` directly from the BLE callback.**
WiFi cannot start while BLE is active. The connection would fail or produce unpredictable behaviour. Fixed: `pass:` now saves credentials to flash, shows `rbt` on the matrix, and calls `ESP.restart()`. WiFi connects cleanly on reboot with no radio conflict.

**`tz:` was attempting `myTZ.setLocation()` while BLE was active.**
`myTZ.setLocation()` requires an active WiFi connection to fetch timezone data. When BLE was running, this call would fail silently and the timezone would not apply. Fixed: `tz:` now always saves to flash and reboots. Timezone is applied on the next boot when WiFi is active.

Both fixes follow the same pattern already used by `reboot:` — notify, show `rbt`, wait 500ms, restart.

### `"rbt"` matrix message restored

`"rbt"` in green was shown before every reboot in v0.12.1. It was accidentally dropped in v0.12.2 when the reboot behaviour was reworked. Now shown consistently before every `ESP.restart()` — `pass:`, `tz:`, and `reboot:` all show it.

### `time?` and `date?` split

The old `time?` response `"H:i:s d/m/Y T"` had two problems:

1. With a real time value like `2:43:37 PM 23/02/2026 MST` it could reach 26+ bytes — over the 20-byte Android BLE WRITE_NR hard limit, causing truncation.
2. It was hardcoded to 24h regardless of the `clock12:`/`clock24:` setting.

Fixed by splitting into two commands:

- **`time?`** — returns time only using `timeFormat()`, matching the display — `14:43` or `2:43 PM`
- **`date?`** — returns date and timezone — `23/02/2026 MST`

Both are safely under 20 bytes.

### Dead code removed

The empty-credentials guard in `syncNTP()` (`if (ssid.length() == 0)`) was unreachable — `setup()` checks for saved credentials before ever calling `syncNTP()`. Removed for clarity.

---

## Upgrade Notes

**Flash as normal** — no partition changes, no Preferences key changes.

If upgrading from v0.12.5: credentials and all saved settings carry over unchanged. No re-configuration needed.

If upgrading from v0.12.1–v0.12.4: same — all Preferences keys are compatible.

**First upload to a new board:** enable Erase All Flash once, flash, then disable. See README for full first-upload instructions.

---

## BLE Command Reference (complete)

| Command | Description |
| --- | --- |
| `ssid:NetworkName` | Buffer SSID (send before `pass:`) |
| `pass:Password` | Save credentials, show `rbt`, reboot |
| `tz:America/Edmonton` | Save timezone, show `rbt`, reboot to apply |
| `time?` | Current time — respects 12h/24h setting |
| `date?` | Current date and timezone string |
| `info?` | Version, settings, uptime, last sync |
| `reboot:` | Show `rbt`, reboot |
| `bright:N` | Brightness level 1–10 |
| `clock12:` | Switch to 12-hour format |
| `clock24:` | Switch to 24-hour format |
| `flip:on` | Flip display 180° (USB top) |
| `flip:off` | Normal orientation (USB bottom) |
| `resync:N` | NTP re-sync interval in hours (1–168) |

All commands must be under 20 bytes — Android BLE WRITE_NR hard limit.

---

## Known Limitations

- No BLE security — pairing is open, planned for v0.13.0
- Non-ASCII SSID names truncate at 20 bytes — use ASCII network names
- Radar animation pixel indices assume USB-bottom orientation — `flip:on` does not rotate the animation
- First cold boot may brownout reset once — second boot always clean
