#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Wi-Fi link manager. Two modes:
//   STATION : joins the network saved in NVS ("wifi" namespace) and keeps retrying
//             forever; announces wifiblu.local (mDNS) once it has an IP.
//   SETUP   : hotspot WIFI_SETUP_SSID + DNS redirector, so a phone that joins it
//             lands on the setup page by itself (captive portal). The station
//             interface stays up for scanning and for trying the entered network.
// Nothing here touches the audio path; the web page (web_ui.c) is the only user.
typedef enum { WIFI_MODE_IDLE, WIFI_MODE_STATION, WIFI_MODE_SETUP } wifi_link_mode_t;

typedef struct {
    wifi_link_mode_t mode;
    bool have_credentials;   // a network is saved in NVS
    bool connected;          // station has an IP
    char ssid[33];           // saved / target network
    char ip[16];             // station IP ("-" when none)
    int rssi;                // dBm, 0 when not connected
    unsigned setup_left_s;   // seconds until the setup hotspot closes (SETUP only)
    char last_result[48];    // "joined", "wrong password", "network not found", ...
} wifi_link_status_t;

esp_err_t wifi_link_init(void);                 // netif + driver; station if saved, setup otherwise
void wifi_link_start_setup(void);               // open the hotspot now (SET hold / console / page)
void wifi_link_stop_setup(void);                // back to station (or idle) mode
esp_err_t wifi_link_join(const char *ssid, const char *password); // save + connect (from the page)
void wifi_link_forget(void);                    // erase saved network, reopen the hotspot
void wifi_link_get_status(wifi_link_status_t *out);
bool wifi_link_in_setup(void);
void wifi_link_log_status(void);

// Scan for networks (blocking, a few seconds). Fills up to max entries; returns the count.
typedef struct { char ssid[33]; int rssi; bool secure; } wifi_link_ap_t;
int wifi_link_scan(wifi_link_ap_t *out, int max);
