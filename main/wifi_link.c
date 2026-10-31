#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "dns_server.h"
#include "board_config.h"
#include "wifi_link.h"

static const char *TAG = "wifi";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *sta_netif, *ap_netif;
static dns_server_handle_t dns;
static esp_timer_handle_t retry_timer, setup_timer;
static bool mdns_up;
static struct {
    wifi_link_mode_t mode;
    bool have_credentials, connected, sta_started, join_pending;
    char ssid[33], password[65], ip[16], last_result[48];
    int rssi;
    int64_t setup_ends_at;
} s = {.ip = "-", .last_result = "-"};

// ------------------------------------------------------------ NVS credentials
static bool load_credentials(void)
{
    nvs_handle_t h; size_t l1 = sizeof(s.ssid), l2 = sizeof(s.password);
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", s.ssid, &l1) == ESP_OK && s.ssid[0];
    if (ok && nvs_get_str(h, "pass", s.password, &l2) != ESP_OK) s.password[0] = 0;
    nvs_close(h);
    return ok;
}

static void save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid); nvs_set_str(h, "pass", password); nvs_commit(h); nvs_close(h);
}

static void erase_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h); nvs_commit(h); nvs_close(h);
}

static void set_result(const char *text)
{
    portENTER_CRITICAL(&lock);
    strncpy(s.last_result, text, sizeof(s.last_result) - 1); s.last_result[sizeof(s.last_result) - 1] = 0;
    portEXIT_CRITICAL(&lock);
}

// --------------------------------------------------------------- mDNS name
static void mdns_start(void)
{
    if (mdns_up) return;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) { ESP_LOGW(TAG, "mDNS init: %s", esp_err_to_name(err)); return; }
    mdns_hostname_set(WIFI_HOSTNAME);
    mdns_instance_name_set("wifi blu speaker");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_up = true;
    ESP_LOGI(TAG, "Control page: http://%s.local/", WIFI_HOSTNAME);
}

// ------------------------------------------------------------- station side
// Must run after every esp_wifi_start() and mode change: the driver resets the cap.
static void apply_tx_power(void)
{
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM);
    int8_t p = 0; esp_wifi_get_max_tx_power(&p);
    ESP_LOGI(TAG, "TX power cap %.2f dBm (%s)", p / 4.0, esp_err_to_name(err));
}

static void apply_sta_config(void)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, s.ssid, sizeof(cfg.sta.ssid));         // both 32 bytes; s.ssid is NUL-terminated at [32]
    memcpy(cfg.sta.password, s.password, sizeof(cfg.sta.password)); // both 64 bytes
    cfg.sta.threshold.authmode = s.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;      // pick the strongest AP of a mesh/repeater network
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true; cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

static void retry_cb(void *arg)
{
    bool want;
    portENTER_CRITICAL(&lock); want = s.have_credentials && !s.connected && s.sta_started; portEXIT_CRITICAL(&lock);
    if (want) {
        ESP_LOGI(TAG, "Trying '%s' again", s.ssid);
        esp_wifi_connect();
    }
}

static void schedule_retry(unsigned seconds)
{
    esp_timer_stop(retry_timer);
    esp_timer_start_once(retry_timer, (uint64_t)seconds * 1000000ULL);
}

static const char *reason_text(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL: case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE: return "wrong password";
    case WIFI_REASON_ASSOC_LEAVE: return "left the network";
    case WIFI_REASON_BEACON_TIMEOUT: return "lost the router";
    default: return "disconnected";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        portENTER_CRITICAL(&lock); s.sta_started = true; portEXIT_CRITICAL(&lock);
        if (s.have_credentials) { ESP_LOGI(TAG, "Joining '%s'", s.ssid); esp_wifi_connect(); }
        break;
    case WIFI_EVENT_STA_STOP:
        portENTER_CRITICAL(&lock); s.sta_started = false; s.connected = false; portEXIT_CRITICAL(&lock);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        bool was_connected, pending;
        portENTER_CRITICAL(&lock);
        was_connected = s.connected; pending = s.join_pending;
        s.connected = false; s.rssi = 0; strcpy(s.ip, "-");
        portEXIT_CRITICAL(&lock);
        const char *why = reason_text(e->reason);
        ESP_LOGW(TAG, "'%s': %s (reason %d)%s", s.ssid, why, e->reason, was_connected ? "" : " while joining");
        if (pending && e->reason == WIFI_REASON_ASSOC_LEAVE) {
            // Our own esp_wifi_disconnect() before the new join: not a failure.
        } else if (pending) {
            // A join from the setup page failed: tell the page, keep the hotspot open,
            // forget the bad password so the next boot does not loop on it.
            char msg[48]; snprintf(msg, sizeof(msg), "failed: %s", why); set_result(msg);
            portENTER_CRITICAL(&lock);
            s.join_pending = false; s.have_credentials = false;
            if (s.mode == WIFI_MODE_STATION) s.mode = WIFI_MODE_IDLE;
            portEXIT_CRITICAL(&lock);
            erase_credentials();
        } else if (s.have_credentials) {
            set_result(why);
            schedule_retry(WIFI_RECONNECT_PERIOD_S);
        }
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "Phone " MACSTR " joined the setup hotspot", MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Phone left the setup hotspot");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *e = data;
    bool pending;
    portENTER_CRITICAL(&lock);
    s.connected = true; pending = s.join_pending; s.join_pending = false;
    snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&e->ip_info.ip));
    portEXIT_CRITICAL(&lock);
    set_result("joined");
    ESP_LOGI(TAG, "Joined '%s', IP %s", s.ssid, s.ip);
    mdns_start();
    if (pending) {
        // Joined from the setup page: give the phone 20 s to show the result, then
        // close the hotspot (SoftAP + Bluetooth on one radio is the unstable mix).
        esp_timer_stop(setup_timer);
        esp_timer_start_once(setup_timer, 20 * 1000000ULL);
        portENTER_CRITICAL(&lock); s.setup_ends_at = esp_timer_get_time() + 20 * 1000000LL; portEXIT_CRITICAL(&lock);
    }
}

// --------------------------------------------------------------- setup mode
static void setup_timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "Setup hotspot closing");
    wifi_link_stop_setup();
}

void wifi_link_start_setup(void)
{
    if (wifi_link_in_setup()) {
        portENTER_CRITICAL(&lock); s.setup_ends_at = esp_timer_get_time() + WIFI_SETUP_TIMEOUT_S * 1000000LL; portEXIT_CRITICAL(&lock);
        esp_timer_stop(setup_timer); esp_timer_start_once(setup_timer, WIFI_SETUP_TIMEOUT_S * 1000000ULL);
        return;
    }
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, WIFI_SETUP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(WIFI_SETUP_SSID);
    strncpy((char *)ap.ap.password, WIFI_SETUP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(WIFI_SETUP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.channel = 6;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power();

    // DHCP option 114 (captive portal URI) for phones that honour it, and a DNS
    // server that answers every name with our own address for the ones that probe.
    esp_netif_ip_info_t info;
    char ip[16] = "192.168.4.1";
    if (esp_netif_get_ip_info(ap_netif, &info) == ESP_OK) inet_ntoa_r(info.ip.addr, ip, sizeof(ip));
    static char uri[32];
    snprintf(uri, sizeof(uri), "http://%s", ip);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(ap_netif);
    if (!dns) {
        dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        dns = start_dns_server(&cfg);
    }
    portENTER_CRITICAL(&lock);
    s.mode = WIFI_MODE_SETUP; s.setup_ends_at = esp_timer_get_time() + WIFI_SETUP_TIMEOUT_S * 1000000LL;
    portEXIT_CRITICAL(&lock);
    esp_timer_stop(setup_timer); esp_timer_start_once(setup_timer, WIFI_SETUP_TIMEOUT_S * 1000000ULL);
    ESP_LOGI(TAG, "SETUP: join hotspot '%s'%s on the phone; the setup page opens by itself (or http://%s/). Closes in %d min",
             WIFI_SETUP_SSID, WIFI_SETUP_PASSWORD[0] ? " (password " WIFI_SETUP_PASSWORD ")" : " (open)", ip, WIFI_SETUP_TIMEOUT_S / 60);
}

void wifi_link_stop_setup(void)
{
    if (!wifi_link_in_setup()) return;
    esp_timer_stop(setup_timer);
    if (dns) { stop_dns_server(dns); dns = NULL; }
    bool creds;
    portENTER_CRITICAL(&lock); creds = s.have_credentials; s.mode = creds ? WIFI_MODE_STATION : WIFI_MODE_IDLE; s.setup_ends_at = 0; portEXIT_CRITICAL(&lock);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // station keeps running; the hotspot goes away
    apply_tx_power();
    ESP_LOGI(TAG, "Setup hotspot closed; %s", creds ? "station mode" : "no network saved (SET hold reopens setup)");
}

bool wifi_link_in_setup(void)
{
    portENTER_CRITICAL(&lock); bool r = s.mode == WIFI_MODE_SETUP; portEXIT_CRITICAL(&lock);
    return r;
}

// ------------------------------------------------------------------ joining
esp_err_t wifi_link_join(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    if (password[0] && strlen(password) < 8) return ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG, "Join request for '%s'", ssid);
    esp_timer_stop(retry_timer);
    portENTER_CRITICAL(&lock);
    strncpy(s.ssid, ssid, sizeof(s.ssid) - 1); s.ssid[32] = 0;
    strncpy(s.password, password, sizeof(s.password) - 1); s.password[64] = 0;
    s.have_credentials = true; s.join_pending = true; s.connected = false;
    if (s.mode == WIFI_MODE_IDLE) s.mode = WIFI_MODE_STATION;
    portEXIT_CRITICAL(&lock);
    save_credentials(ssid, password);
    set_result("joining...");
    esp_wifi_disconnect();
    apply_sta_config();
    return esp_wifi_connect();
}

void wifi_link_forget(void)
{
    ESP_LOGI(TAG, "Forgetting the saved network");
    esp_timer_stop(retry_timer);
    portENTER_CRITICAL(&lock);
    s.have_credentials = false; s.connected = false; s.join_pending = false; s.ssid[0] = 0; s.password[0] = 0; strcpy(s.ip, "-");
    if (s.mode == WIFI_MODE_STATION) s.mode = WIFI_MODE_IDLE;
    portEXIT_CRITICAL(&lock);
    erase_credentials();
    esp_wifi_disconnect();
    set_result("-");
    wifi_link_start_setup();
}

int wifi_link_scan(wifi_link_ap_t *out, int max)
{
    wifi_scan_config_t cfg = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = {.min = 60, .max = 120}};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) { ESP_LOGW(TAG, "Scan: %s", esp_err_to_name(err)); return 0; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    if (n > 24) n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) { esp_wifi_clear_ap_list(); return 0; }
    esp_wifi_scan_get_ap_records(&n, recs);
    int count = 0;
    for (int i = 0; i < n && count < max; ++i) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < count; ++k) if (!strcmp(out[k].ssid, (char *)recs[i].ssid)) { dup = true; break; }
        if (dup) continue; // several APs of one network: keep the strongest (list is sorted by RSSI)
        strncpy(out[count].ssid, (char *)recs[i].ssid, 32); out[count].ssid[32] = 0;
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        ++count;
    }
    free(recs);
    return count;
}

// ------------------------------------------------------------------- status
void wifi_link_get_status(wifi_link_status_t *o)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    o->mode = s.mode; o->have_credentials = s.have_credentials; o->connected = s.connected;
    strcpy(o->ssid, s.ssid); strcpy(o->ip, s.ip); strcpy(o->last_result, s.last_result);
    o->setup_left_s = (s.mode == WIFI_MODE_SETUP && s.setup_ends_at > now) ? (unsigned)((s.setup_ends_at - now) / 1000000) : 0;
    portEXIT_CRITICAL(&lock);
    o->rssi = 0;
    if (o->connected) esp_wifi_sta_get_rssi(&o->rssi);
}

void wifi_link_log_status(void)
{
    wifi_link_status_t st; wifi_link_get_status(&st);
    static const char *modes[] = {"idle (no network saved)", "station", "SETUP hotspot"};
    ESP_LOGI(TAG, "Status: %s, network '%s' %s, IP %s, RSSI %d dBm, last: %s%s",
             modes[st.mode], st.ssid[0] ? st.ssid : "-", st.connected ? "CONNECTED" : "not connected",
             st.ip, st.rssi, st.last_result, mdns_up ? ", wifiblu.local announced" : "");
}

// --------------------------------------------------------------------- init
esp_err_t wifi_link_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta_netif, WIFI_HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // credentials live in our own NVS namespace
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
    esp_timer_create_args_t a = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&a, &retry_timer));
    esp_timer_create_args_t b = {.callback = setup_timeout_cb, .name = "wifi_setup"};
    ESP_ERROR_CHECK(esp_timer_create(&b, &setup_timer));
    // Modem sleep stays on (default): the Bluetooth side gets the radio more often.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    s.have_credentials = load_credentials();
    if (s.have_credentials) {
        s.mode = WIFI_MODE_STATION;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        apply_sta_config();
        ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power();
        ESP_LOGI(TAG, "Saved network '%s': joining as station (SET hold %d s or 'W' opens the setup hotspot)", s.ssid, WIFI_SETUP_HOLD_S);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    apply_tx_power();
        ESP_LOGI(TAG, "No network saved: opening the setup hotspot");
        wifi_link_start_setup();
    }
    return ESP_OK;
}
