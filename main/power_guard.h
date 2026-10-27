#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Brownout guard: the ESP32's own brownout comparator watches the 3.3 V rail
// with microsecond latency. ESP-IDF's default handler resets the chip; here
// the interrupt instead cuts the amplifier enable line at once (the load that
// is pulling the supply down), so the rail recovers before the LyraT's IMP809
// supervisor (2.63 V) can reset the board and the Bluetooth link survives.
// The audio task then lowers the DAC profile one step, saves it, and re-arms
// the detector after a short hold-off. CONFIG_ESP_BROWNOUT_DET is disabled so
// the IDF handler does not take the interrupt first.
esp_err_t power_guard_init(void);
bool power_guard_take_event(void);  // true once per brownout event (clears it)
uint32_t power_guard_events(void);  // total events since boot
void power_guard_rearm(void);       // re-enable the interrupt after the hold-off
