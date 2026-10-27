# wifiblu: LyraT Bluetooth speaker + calls, now with Wi-Fi

Project 3 in this workspace. Started 2026-10-27 from the final state of the **bleaudiocall** project (ESP32-LyraT V4.3, ESP-IDF v6.1): A2DP music, AVRCP, HFP hands-free calls, reconnect logic, power self-test and brownout guard. This project adds **Wi-Fi**.

Bluetooth name: `wifi blu` (`main/board_config.h`). CMake project name: `wifiblu` (no spaces in folder or project names: ESP-IDF on Windows does not support them).

## What is carried over

- `main/`: the complete bleaudiocall firmware (see that project's docs for the design: `docs/01`..`docs/09` there).
- `tools/`: Build / Flash / Monitor scripts (COM6), serial capture, log plotter.
- `docs/lyrat-v4.3-hardware.md`, `docs/hardware/` (official schematic, layout, BOM), `docs/references/` (ES8388 reference driver).
- `sdkconfig.defaults`: same as bleaudiocall plus `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` (Wi-Fi and Classic Bluetooth share the one radio).

## Wi-Fi plan (to be decided)

Open questions before writing code: what Wi-Fi is for (phone app / web page for settings, OTA firmware update, internet radio, multi-room), station or access point, how credentials get in (SmartConfig, captive portal, serial). Constraint to remember: on the ESP32 the radio is shared; Wi-Fi traffic during A2DP streaming causes audio drop-outs unless Wi-Fi is kept idle while music plays.

## Build / flash / monitor

Same as bleaudiocall: open this folder in VS Code (terminal profile activates ESP-IDF v6.1), `Ctrl+Shift+B` to build, task "ESP32: Flash wifiblu (COM6)", task "ESP32: Live call/music logs (COM6)".

## Changelog

| Date | Change | Result |
|---|---|---|
| 2026-10-27 | Project created from bleaudiocall v1.0.1; name `wifi blu`; coexistence enabled in config; no Wi-Fi code yet | not built yet |
