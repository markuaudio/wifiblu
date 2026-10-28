# wifiblu: LyraT Bluetooth speaker + calls, now with Wi-Fi

Project 3 in this workspace. Started 2026-10-27 from the final state of the **bleaudiocall** project (ESP32-LyraT V4.3, ESP-IDF v6.1): A2DP music, AVRCP, HFP hands-free calls, reconnect logic, power self-test and brownout guard. This project adds **Wi-Fi**.

Bluetooth name: `wifi blu` (`main/board_config.h`). CMake project name: `wifiblu` (no spaces in folder or project names: ESP-IDF on Windows does not support them).

## What is carried over

- `main/`: the complete bleaudiocall firmware (see that project's docs for the design: `docs/01`..`docs/09` there).
- `tools/`: Build / Flash / Monitor scripts (COM6), serial capture, log plotter.
- `docs/lyrat-v4.3-hardware.md`, `docs/hardware/` (official schematic, layout, BOM), `docs/references/` (ES8388 reference driver).
- `sdkconfig.defaults`: same as bleaudiocall plus `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` (Wi-Fi and Classic Bluetooth share the one radio).

## Wi-Fi (Phase 1, built 2026-10-28, not yet tested on the board)

Research and the reasons for this scope: `docs/00-wifi-options.md`. Design, behaviour and the test list: `docs/01-wifi-phase1.md`. General ideas (market, audio projects) live in `../ideas-esp/`.

- **Setup**: with no network saved (or SET held 6 s, or console `W`) the speaker opens the hotspot `wifi blu setup`; the phone that joins it gets the setup page by itself (captive portal). Pick the home network, type the password, done. Station mode afterwards, hotspot closed.
- **Control page**: `http://wifiblu.local/` (mDNS) on the home network: Bluetooth / call / Wi-Fi status, play, next, volume, answer, hang up, connect, disconnect, self-test, forget phones, forget Wi-Fi, reboot, free memory, uptime.
- **Firmware update**: upload `build/wifiblu.bin` from the page. Two OTA slots (`partitions.csv`), rollback if the new image does not come up.
- Radio rule: Wi-Fi station + Bluetooth is the stable mix; the hotspot runs only during setup; uploads share the radio with music (pause first).

Console keys added: `w` Wi-Fi status, `W` setup hotspot, `n` forget network.

## Build / flash / monitor

Same as bleaudiocall: open this folder in VS Code (terminal profile activates ESP-IDF v6.1), `Ctrl+Shift+B` to build, task "ESP32: Flash wifiblu (COM6)", task "ESP32: Live call/music logs (COM6)".

## Changelog

| Date | Change | Result |
|---|---|---|
| 2026-10-27 | Project created from bleaudiocall v1.0.1; name `wifi blu`; coexistence enabled in config; no Wi-Fi code yet | builds, 824 KB |
| 2026-10-28 | Wi-Fi Phase 1: setup hotspot + captive portal, `wifiblu.local` control page (esp_http_server + mDNS), OTA upload with rollback, OTA partition table, SET 6 s hold, console keys | builds, 1.49 MB (53% of the 3 MB slot free); board not connected, untested (test list in docs/01) |
