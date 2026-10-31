# 01: Wi-Fi Phase 1: setup hotspot, wifiblu.local control page, OTA update

Written 2026-10-28 with the first build. **Not yet tested on the board** (the LyraT was not connected). The test list at the end is the next step.

## What was added

| File | Role |
|---|---|
| `main/wifi_link.c/.h` | Wi-Fi link manager: station mode with the saved network (NVS namespace `wifi`), retry every 10 s forever; setup mode = hotspot `wifi blu setup` + DNS redirector (captive portal); scan; join; forget; mDNS `wifiblu.local` once an IP arrives. |
| `main/web_ui.c/.h` | HTTP server on port 80: the page, `/api/status` JSON, `/api/cmd`, `/api/scan`, `/api/wifi`, `/update` (OTA), captive-portal 404 redirect. |
| `main/web/index.html` | One page (embedded in the binary): status, music/call controls, Wi-Fi setup (shown only in setup mode), firmware upload with progress, device actions. Polls `/api/status` every 2 s. Light/dark, phone-sized. |
| `components/dns_server/` | Copied from the ESP-IDF captive-portal example: answers every DNS query with the hotspot's own address. |
| `main/idf_component.yml` | Pulls `espressif/mdns` from the component registry (mDNS is no longer inside ESP-IDF). `managed_components/` is generated and ignored by git; `dependencies.lock` is committed. |
| `partitions.csv` | `nvs`, `otadata`, `ota_0` (3 MB), `ota_1` (3 MB). No factory slot: the cable flash goes into `ota_0`. NVS keeps its offset, so Bluetooth bonds and the DAC limit survive the change. |
| `sdkconfig.defaults` | `BOOTLOADER_APP_ROLLBACK_ENABLE`, 16 sockets, smaller Wi-Fi buffers (Bluetooth already owns a lot of RAM), AMPDU off. |
| `main/main.c` | Starts Wi-Fi after Bluetooth, starts the web server, confirms a new OTA image after `OTA_VALID_AFTER_S` (45 s) only if Bluetooth came up; console keys `w` status, `W` setup, `n` forget network, `i` now includes Wi-Fi. |
| `main/touch_buttons.c` | SET held 6 s (`WIFI_SETUP_HOLD_S`) opens the setup hotspot. The 2 s disconnect still fires on the way. |
| `main/bt_core.c`, `main/audio_engine.c` | Small status getters for the page (`bt_core_get_status`, `audio_engine_dac_profile`). |

Settings live in `main/board_config.h` (Wi-Fi block at the end): hostname, hotspot name and password, timeouts.

## How it behaves

1. **First boot, nothing saved**: hotspot `wifi blu setup` (open) appears. Phone joins it, the setup page opens by itself (DHCP option 114 + DNS redirect + 302 on every unknown URL; iOS, Android and Windows all probe differently, all three are covered). The page scans, the user picks the network and types the password, taps Join.
2. **Join result**: "joined" with the IP, or "failed: wrong password / network not found". A failed password is erased so the next boot does not loop on it. On success the hotspot closes 20 s later and the device is reachable at `http://wifiblu.local/` on the home network.
3. **Normal boot with a saved network**: station only, no hotspot. Lost router: retry every 10 s, forever, quietly.
4. **Reopen setup**: hold SET 6 s, console `W`, or the page's "Open setup hotspot". It closes by itself after 10 min.
5. **Firmware update**: page > Firmware update > choose `build/wifiblu.bin` > Upload. The image goes to the other OTA slot, the device reboots into it. The new image confirms itself after 45 s if Bluetooth is up; if it crashes or never gets there, the next reset boots the previous image. Uploads are refused during a call.
6. **Coexistence**: station mode next to Bluetooth is the stable combination. The hotspot runs only during setup. The page's polling is light. Music will stutter during a firmware upload: the page says to pause first.

## Memory and radio: what to measure first

- Free heap at boot (`Initialized; running from ota_0; free heap=...`) and minimum free heap on the page after 10 minutes of music + page open. If it drops under about 30 KB, turn PSRAM on for the music ring buffer.
- A2DP during page polling: no drop-outs expected. A2DP during a scan: short gap acceptable (setup only).
- HFP call while the page is open: acceptable; scan and upload are refused during a call.

## Test list (board on COM6)

| # | Test | Expect |
|---|---|---|
| T1 | Flash with cable (`idf.py -p COM6 flash`): new partition table | boots from `ota_0`, bonds still there, DAC limit still there |
| T2 | No network saved | log shows SETUP, phone sees `wifi blu setup`, page pops up on Android and iPhone |
| T3 | Wrong password | page shows "failed: wrong password", hotspot stays open, nothing saved |
| T4 | Right password | "joined" + IP, hotspot closes after 20 s, `http://wifiblu.local/` works from the home network (Android Chrome may need the IP; mDNS on Android works in most browsers now) |
| T5 | Reboot | joins by itself, no hotspot |
| T6 | Page controls while music plays | play/pause/next/volume follow, phone's volume slider follows (AVRCP) |
| T7 | Call from the page | Answer / Hang up buttons work during an incoming call |
| T8 | OTA: bump `CONFIG_APP_PROJECT_VER`, build, upload from the page | reboots into `ota_1`, page shows the new version and slot, 45 s later log says "confirmed" |
| T9 | OTA rollback: upload a build that panics early (temporary `abort()` in `app_main`) | device reboots twice, then runs the previous image again |
| T10 | Heap | boot heap and page minimum heap written into the changelog |
| T11 | SET hold 6 s during music | disconnect at 2 s, hotspot at 6 s |
| T12 | Hotspot timeout | closes after 10 min by itself |

## Test run 2026-10-30 (first flash)

Board on COM6, powered only through the UART Micro-USB from the PC (no charger on the POWER port).

- Flash with cable: OK. New partition table written, bootloader loads from `ota_0` (offset 0x20000), project name `wifiblu`.
- Boot stops at `phy_init: phy_version ...` (radio power-on) and the chip restarts with `rst:0x1 (POWERON_RESET)`, about once per second, 37 times in 40 s. No panic, no backtrace, no brownout message: the IMP809 supervisor pulls EN when the 3.3 V rail collapses (see bleaudiocall `docs/07`).
- A/B check: the released bleaudiocall v1.0.1 image, which ran fine on 2026-10-27, was flashed back and shows the same loop at the same point. So the loop is the supply, not the Wi-Fi code. wifiblu was flashed again afterwards; the board is left with wifiblu in `ota_0`.
- Possible aggravating factor: the `phy_init` data partition moved from 0xf000 to 0x11000, so RF calibration data is empty and a full calibration (longer, higher current) runs on every boot until one boot completes.

- Second round, same day, after the user changed the supply: still the same loop (reset 0.28 s after `phy_version`, cycle 1.1 s; the 0.28 s matches the IMP809 reset hold, so the supervisor resets the chip, the charger does not drop out).
- `hello_world` (ESP-IDF example, no radio) flashed for comparison: runs for 30 s with no unexpected reset, 304 KB free heap. The board itself is fine; only the radio power-on kills the rail.
- Software mitigation tried: `CONFIG_ESP_PHY_RF_CAL_NONE` + `CONFIG_ESP_PHY_MAX_TX_POWER=8` (no boot-time RF calibration, lower TX power). No change at all: 41 resets in 45 s. Reverted; the committed `sdkconfig` is untouched and the board holds the unmodified wifiblu build (verified with `esptool verify-flash`).
- Conclusion: nothing in firmware can fix this. The 5 V -> 3.3 V path on the board (D3 1N5819, Q2, power switch, LD1117; see bleaudiocall `docs/07`) cannot deliver the radio start-up current. Either the supply/cable is still weak, or that path was degraded by the earlier high-current amplifier sessions. A multimeter on the 3.3 V and 5 V rails during boot would settle it.


## Test run 2026-10-30, third round (PSRAM)

- Build + flash via `tools/Build.ps1` / `tools/Flash.ps1` (COM6). The board now gets past `phy_init` on every other boot (the previous boot's reset leaves the rail charged), so the firmware could be watched running for the first time.
- Firmware bug found: with Bluedroid Classic + Wi-Fi AP/STA + lwIP up, internal heap before `web_ui_start()` was **4.5 KB** (largest block 4.3 KB), so `httpd_start` failed with `ESP_ERR_HTTPD_TASK` and `ESP_ERROR_CHECK` aborted the app (`SW_CPU_RESET` loop). Static RAM per component (`idf.py size-components`): Bluedroid 41 KB, net80211 11 KB.
- Fix: PSRAM enabled, as planned in this doc (`CONFIG_SPIRAM`, `SPIRAM_USE_MALLOC`, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `BT_ALLOCATION_FROM_SPIRAM_FIRST`, `ESP32_REV_MIN_3` so the cache workaround is not compiled in; Bluedroid `BT_ACL_CONNECTIONS` 4 -> 2). The music ring buffer is created in PSRAM (`xRingbufferCreateWithCaps`, falls back to internal RAM). `web_ui_start()` failure is now logged, not fatal: Bluetooth audio keeps working without the page. Boot log prints internal + PSRAM heap before the web server.
- Result (`docs/2026-10-30-run-psram.log`): `Found 8MB PSRAM device` (4 MB mapped, normal on ESP32), memory test OK, min chip rev v3.0. Internal heap before web server **57 KB** (largest block 56 KB), PSRAM 4.0 MB free. `Control page on port 80`, DNS redirector on 53, hotspot `wifi blu setup` up, Bluetooth READY + discoverable, reconnect to the last phone attempted. Internal heap after init 44 KB.
- Still resetting at this point: the brownout guard fires every 410 ms (3.3 V rail at 2.80 V with the amplifier off) and the IMP809 resets the chip after 12-21 s (`POWERON_RESET`, 3 in 40 s). Cause found in the fourth round below: Wi-Fi transmit power, not the supply.
- Image size 1.50 MB (53% of the 3 MB slot free). Not committed yet.


## Test run 2026-10-30, fourth round (Wi-Fi TX power): runs clean

- The user pointed out that bleaudiocall v1.0.1 ran on this same cable and supply. A/B on the spot (`docs/2026-10-30-ab-bleaudiocall-v101.log`): the v1.0.1 release image runs 40 s clean, zero brownout-guard trips apart from the power self-test tone at P4. So the supply carries the Bluetooth-only load; what it cannot carry is Bluetooth **plus** Wi-Fi at the chip's default 19.5 dBm transmit power (AP beacons every 100 ms + continuous receive). The earlier "not firmware" conclusion in rounds 1-3 was wrong: the rail was marginal all along and the full-power Wi-Fi bursts pushed it over.
- Fix: `WIFI_MAX_TX_POWER_QDBM` in `main/board_config.h`, applied with `esp_wifi_set_max_tx_power()` after every `esp_wifi_start()` and mode change (`apply_tx_power()` in `main/wifi_link.c`, logs the cap). Data: 34 (8.5 dBm) 40 s + 120 s clean, 0 resets, 0 guard trips (`docs/2026-10-30-run-txpower8.log`, `-120s.log`); 44 (11 dBm) 60 s clean (`docs/2026-10-30-run-txpower11.log`). **44 is the default** (keeps range for the station link; 9 dB below the chip maximum). The driver reports 10.5 dBm for 44 and 8.25 dBm for 34.
- State of the board now: wifiblu in `ota_0` with PSRAM + 11 dBm cap, Bluetooth discoverable, hotspot `wifi blu setup` open, control page on port 80. Idle test only (no phone paired, no music): the music + amplifier load is still covered by the power self-test and the brownout guard from v1.0.1.
- Not committed yet.

**Next:** T1 (bonds, DAC limit) and T2 (join `wifi blu setup` from the phone, page at http://192.168.4.1/), then T3-T12. Watch `brownout guard events since boot` during music with the hotspot open; if it climbs, drop `WIFI_MAX_TX_POWER_QDBM` to 34. A 5 V 2 A charger on the POWER port still gives more headroom for loud music.

## Not in Phase 1 (hardening, next)

Page password; signed OTA images (secure boot) + flash encryption; factory reset from a button; production self-test page; per-unit name from the MAC.
