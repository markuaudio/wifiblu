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

## Not in Phase 1 (hardening, next)

Page password; signed OTA images (secure boot) + flash encryption; factory reset from a button; production self-test page; per-unit name from the MAC.
