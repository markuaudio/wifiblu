#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Bluetooth controller + Bluedroid host, GAP (name, class of device, pairing
// for every phone generation) and the link manager that opens A2DP/HFP by
// itself when a phone does not.
typedef enum { PROFILE_A2DP, PROFILE_HFP } bt_profile_t;

esp_err_t bt_core_radio_init(void); // controller + host; call before codec/I2S init
esp_err_t bt_core_start(void);      // GAP setup, profiles, link manager task
void bt_core_profile_ready(bt_profile_t profile);
void bt_core_profile_link(bt_profile_t profile, bool up, const uint8_t *bda);
void bt_core_profile_failed(bt_profile_t profile); // our own connect attempt ended without a link
bool bt_core_connected(void);       // any profile up
bool bt_core_ready(void);           // A2DP + HFP initialised (GAP usable)
void bt_core_radio_load(bool on);   // power self-test: run a Bluetooth inquiry to load the radio like streaming does
void bt_core_disconnect(void);      // user request: drop both profiles, stop auto-reconnect
void bt_core_connect_last(void);    // user request: reconnect to the newest bonded phone
void bt_core_forget_all(void);      // remove every bond (phone must pair again)
void bt_core_list_bonds(void);
void bt_core_log_status(void);
typedef struct {
    bool music_up, calls_up, discoverable, have_peer;
    char peer[18], own[18];
    unsigned reconnect_attempts;
    int bonds;
} bt_status_t;
void bt_core_get_status(bt_status_t *out); // snapshot for the web page
const char *bt_core_bda_str(const uint8_t *bda, char out[18]);
