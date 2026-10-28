# 00: What Wi-Fi can add to wifiblu (research, 2026-10-27, decision pending)

Written after reading Espressif's docs for ESP-IDF v6.1 on the ESP32-LyraT V4.3. Kept in simple language so it can be re-read before deciding.

## Two facts that shape every option

1. **One antenna.** The ESP32 shares its single radio between Classic Bluetooth and Wi-Fi, in turns. Espressif's coexistence table says: Wi-Fi *station* connected + Bluetooth connected = stable; Wi-Fi *SoftAP* connected + Bluetooth = unstable; and heavy Wi-Fi traffic during an HFP call gives poor call audio "as expected".
   Rule for this project: Wi-Fi in station mode in normal use, SoftAP only for setup, and only light traffic (settings page, update) while music or a call is running. No downloads next to a stream.
2. **ESP-ADF cannot be used.** Espressif's audio framework supports ESP-IDF v5.0 to v5.3 only and marks IDF master/v6 as not supported. Everything here is built with plain ESP-IDF v6.1 plus component-registry components; no ready-made ADF pipelines (internet radio, DLNA).

## Options

| # | Feature | How (Espressif piece) | Fit | Size |
|---|---|---|---|---|
| 1 | Give the device the Wi-Fi password (setup) | Device opens its own hotspot "wifi blu setup"; phone joins it and a page opens by itself (captive portal, IDF example `protocols/http_server/captive_portal`, ESP32 supported). No app needed. Alternatives: SmartConfig/EspTouch (needs Espressif app), Unified Provisioning SoftAP (needs ESP SoftAP Prov app, manual join on iOS), BLE provisioning (we are BR/EDR-only; BLE costs about 110 KB RAM: no). | best: captive portal | small |
| 2 | Firmware update without a cable (OTA) | Upload the .bin from a browser page, or `esp_https_ota` from a URL. Partition table changes to otadata + ota_0 + ota_1 (8 MB flash: two 3 MB slots). Rollback is on by default: a bad image reboots back to the old one. Replaces the client flashing procedure of bleaudiocall doc 10. | biggest value for the client | medium |
| 3 | Control page in the browser | `esp_http_server` + mDNS (`wifiblu.local`): connected phone, volume, power/DAC status, bond list and forget, self-test, everything the serial console keys do today. Live log over WebSocket for field debugging without a cable. | good, light traffic | medium |
| 4 | Internet radio (HTTP MP3/AAC) | `esp_audio_codec` component supports ESP32 (MP3 about 28 KB heap, AAC about 51 KB, 20 KB task stack). New source inside audio_engine; continuous Wi-Fi traffic, so only while Bluetooth is idle. | possible, large, coexistence risk | large |
| 5 | Multi-room / synced speakers | ESP-NOW or UDP. Forum reports show 45 ms send delays next to A2DP. | not on this chip with BT | very large |
| 6 | Voice assistant / echo cancel (AEC) | esp-sr dropped the original ESP32; ADF unusable. | not possible | — |
| 7 | Cloud control (ESP RainMaker, Alexa/Google on-off) | Works on ESP32, needs account linking and a server. | maybe later | medium |
| 8 | Small extras | SNTP real time in logs, heap/uptime telemetry on the page. | comes free with 3 | small |

## Recommendation (not yet decided)

Phase 1 = 1 + 2 + 3 together. All three sit on one small web server inside the device, keep Wi-Fi traffic light, and give the client immediate value: easy setup, cable-free update, browser control.

Flow: first boot (or SET hold) with no saved Wi-Fi -> hotspot + captive portal -> user picks home Wi-Fi -> device joins as station -> `wifiblu.local` page with status, control and "Update firmware". Music and calls work exactly as before.

Phase 2 (optional, separate): internet radio.

Risk to check on the first build: free heap with Bluedroid Classic + Wi-Fi + lwIP all up. The WROVER module's PSRAM is off in the config and can be enabled for buffers if RAM runs short.

## Sources

- ESP-IDF RF coexistence (ESP32): https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/coexist.html
- ESP-FAQ coexistence: https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/coexistence.html
- Unified provisioning: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/provisioning/provisioning.html
- OTA: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html
- esp_local_ctrl: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_local_ctrl.html
- ESP-ADF README, IDF support table: https://github.com/espressif/esp-adf/blob/master/README.md
- esp_audio_codec: https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.4.1/readme
- esp-sr changelog (ESP32 dropped): https://components.espressif.com/components/espressif/esp-sr/versions/2.5.2/changelog
- ESP RainMaker voice assistants: https://docs.rainmaker.espressif.com/docs/dev/firmware/fw_usage_guides/voice-assistant-usage
