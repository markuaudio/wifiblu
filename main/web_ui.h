#pragma once
#include "esp_err.h"

// HTTP server on port 80: the control page (main/web/index.html), a small JSON
// API the page polls, Wi-Fi setup (scan / join / forget), firmware upload (OTA)
// and the captive-portal redirects used while the setup hotspot is open.
//   GET  /                 control + setup page
//   GET  /api/status       JSON snapshot (Bluetooth, call, audio, Wi-Fi, system)
//   POST /api/cmd?c=NAME   play next volup voldown vol(&v=0..100) answer hangup
//                          connect disconnect forget_bonds selftest wifi_setup
//                          wifi_forget reboot
//   GET  /api/scan         JSON list of nearby networks (blocking, ~2 s)
//   POST /api/wifi         body ssid=...&pass=... (form encoded) -> join
//   POST /update           raw .bin body -> next OTA slot, then reboot
esp_err_t web_ui_start(void);
