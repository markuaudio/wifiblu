#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "board_config.h"
#include "app_keys.h"
#include "audio_engine.h"
#include "bt_core.h"
#include "bt_music.h"
#include "bt_call.h"
#include "wifi_link.h"
#include "web_ui.h"

static const char *TAG = "web";
static httpd_handle_t server;
extern const char index_start[] asm("_binary_index_html_start");
extern const char index_end[] asm("_binary_index_html_end");

// ------------------------------------------------------------------ helpers
static void url_decode(char *str)
{
    char *w = str;
    for (char *r = str; *r; ++r) {
        if (*r == '+') *w++ = ' ';
        else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16); r += 2;
        } else *w++ = *r;
    }
    *w = 0;
}

// JSON string escape for SSIDs and names typed by people.
static void json_str(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    for (; *in && n + 2 < cap; ++in) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { if (n + 3 >= cap) break; out[n++] = '\\'; out[n++] = c; }
        else if (c < 0x20) { if (n + 7 >= cap) break; n += snprintf(out + n, cap - n, "\\u%04x", c); }
        else out[n++] = c;
    }
    out[n] = 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t total = req->content_len, got = 0;
    if (total >= cap) return ESP_ERR_INVALID_SIZE;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[got] = 0;
    return ESP_OK;
}

// ------------------------------------------------------------------- pages
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_start, index_end - index_start);
}

// Captive portal: while the setup hotspot is open every unknown URL (the
// phones' connectivity probes included) is redirected to the page. iOS needs a
// body, Android needs a redirect, Windows needs anything but its expected text.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (!wifi_link_in_setup()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "wifiblu: no such page");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "wifi blu setup", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --------------------------------------------------------------- /api/status
static esp_err_t status_get(httpd_req_t *req)
{
    bt_status_t bt; bt_core_get_status(&bt);
    wifi_link_status_t wf; wifi_link_get_status(&wf);
    static const char *calls[] = {"idle", "incoming", "outgoing", "active"};
    static const char *modes[] = {"idle", "station", "setup"};
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char ssid[80]; json_str(ssid, sizeof(ssid), wf.ssid);
    char body[768];
    snprintf(body, sizeof(body),
             "{\"bt\":{\"music\":%s,\"calls\":%s,\"discoverable\":%s,\"peer\":\"%s\",\"bonds\":%d,\"own\":\"%s\",\"attempts\":%u},"
             "\"call\":{\"state\":\"%s\",\"volume\":%u},"
             "\"audio\":{\"volume\":%u,\"dac\":%u,\"in_call\":%s},"
             "\"wifi\":{\"mode\":\"%s\",\"saved\":%s,\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"setup_left\":%u,\"result\":\"%s\",\"host\":\"%s.local\"},"
             "\"sys\":{\"version\":\"%s\",\"slot\":\"%s\",\"heap\":%lu,\"heap_min\":%lu,\"uptime\":%lld,\"name\":\"%s\"}}",
             bt.music_up ? "true" : "false", bt.calls_up ? "true" : "false", bt.discoverable ? "true" : "false",
             bt.peer, bt.bonds, bt.own, bt.reconnect_attempts,
             calls[bt_call_state()], audio_engine_call_get_volume(),
             audio_engine_music_get_volume(), audio_engine_dac_profile(), audio_engine_call_active() ? "true" : "false",
             modes[wf.mode], wf.have_credentials ? "true" : "false", wf.connected ? "true" : "false",
             ssid, wf.ip, wf.rssi, wf.setup_left_s, wf.last_result, WIFI_HOSTNAME,
             app->version, run ? run->label : "?", (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(), (long long)(esp_timer_get_time() / 1000000), BT_DEVICE_NAME);
    return send_json(req, body);
}

// ------------------------------------------------------------------ /api/cmd
static void reboot_cb(void *arg) { esp_restart(); }
static void reboot_soon(unsigned ms)
{
    esp_timer_handle_t t;
    esp_timer_create_args_t a = {.callback = reboot_cb, .name = "reboot"};
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static esp_err_t cmd_post(httpd_req_t *req)
{
    char query[96] = "", c[32] = "", v[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "c", c, sizeof(c));
    httpd_query_key_value(query, "v", v, sizeof(v));
    ESP_LOGI(TAG, "Command '%s' %s", c, v);
    const char *reply = "{\"ok\":true}";
    if (!strcmp(c, "play")) app_key(KEY_PLAY);
    else if (!strcmp(c, "next")) app_key(KEY_NEXT);
    else if (!strcmp(c, "volup")) app_key(KEY_VOL_UP);
    else if (!strcmp(c, "voldown")) app_key(KEY_VOL_DOWN);
    else if (!strcmp(c, "vol")) {
        int pct = atoi(v); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        // Step through the music volume so AVRCP keeps the phone's slider in sync.
        int cur = (int)audio_engine_music_get_volume();
        for (int guard = 0; guard < 20 && abs(cur - pct) > 3; ++guard) {
            app_key(cur < pct ? KEY_VOL_UP : KEY_VOL_DOWN);
            int next = (int)audio_engine_music_get_volume();
            if (next == cur) break;
            cur = next;
        }
    }
    else if (!strcmp(c, "answer")) bt_call_answer();
    else if (!strcmp(c, "hangup")) bt_call_hangup();
    else if (!strcmp(c, "connect")) bt_core_connect_last();
    else if (!strcmp(c, "disconnect")) bt_core_disconnect();
    else if (!strcmp(c, "forget_bonds")) bt_core_forget_all();
    else if (!strcmp(c, "selftest")) audio_engine_calibrate();
    else if (!strcmp(c, "wifi_setup")) wifi_link_start_setup();
    else if (!strcmp(c, "wifi_forget")) wifi_link_forget();
    else if (!strcmp(c, "reboot")) { reply = "{\"ok\":true,\"note\":\"rebooting\"}"; reboot_soon(500); }
    else { httpd_resp_set_status(req, "400 Bad Request"); reply = "{\"ok\":false,\"error\":\"unknown command\"}"; }
    return send_json(req, reply);
}

// ----------------------------------------------------------------- /api/scan
static esp_err_t scan_get(httpd_req_t *req)
{
    if (audio_engine_call_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"not during a call\"}");
    }
    wifi_link_ap_t aps[16];
    int n = wifi_link_scan(aps, 16);
    char *body = malloc(2048);
    if (!body) return httpd_resp_send_500(req);
    size_t len = snprintf(body, 2048, "{\"ok\":true,\"networks\":[");
    for (int i = 0; i < n && len < 1900; ++i) {
        char ssid[80]; json_str(ssid, sizeof(ssid), aps[i].ssid);
        len += snprintf(body + len, 2048 - len, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                        i ? "," : "", ssid, aps[i].rssi, aps[i].secure ? "true" : "false");
    }
    snprintf(body + len, 2048 - len, "]}");
    esp_err_t r = send_json(req, body);
    free(body);
    return r;
}

// ----------------------------------------------------------------- /api/wifi
static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[200], ssid[40] = "", pass[80] = "";
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"error\":\"bad body\"}");
    }
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid); url_decode(pass);
    esp_err_t err = wifi_link_join(ssid, pass);
    if (err == ESP_ERR_INVALID_SIZE) { httpd_resp_set_status(req, "400 Bad Request"); return send_json(req, "{\"ok\":false,\"error\":\"password needs 8+ characters\"}"); }
    if (err != ESP_OK) { httpd_resp_set_status(req, "400 Bad Request"); return send_json(req, "{\"ok\":false,\"error\":\"bad network name\"}"); }
    return send_json(req, "{\"ok\":true,\"note\":\"joining\"}");
}

// -------------------------------------------------------------------- /update
// Raw application image in the request body, written straight into the OTA
// slot that is not running. On success the device reboots into it; the new
// image must confirm itself (main.c) or the bootloader comes back here.
static esp_err_t update_post(httpd_req_t *req)
{
    if (audio_engine_call_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"ok\":false,\"error\":\"not during a call\"}");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || req->content_len < 64 * 1024 || req->content_len > target->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"error\":\"not a firmware image\"}");
    }
    ESP_LOGI(TAG, "Firmware upload: %u bytes into %s", (unsigned)req->content_len, target->label);
    char *buf = malloc(4096);
    if (!buf) return httpd_resp_send_500(req);
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &ota);
    size_t got = 0; int64_t t0 = esp_timer_get_time(); int timeouts = 0;
    while (err == ESP_OK && got < req->content_len) {
        int r = httpd_req_recv(req, buf, 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue; // up to ~60 s of silence
        if (r <= 0) { err = ESP_ERR_TIMEOUT; break; }
        timeouts = 0;
        err = esp_ota_write(ota, buf, r);
        got += r;
    }
    free(buf);
    if (err == ESP_OK) err = esp_ota_end(ota); else if (ota) esp_ota_abort(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Firmware upload failed after %u bytes: %s", (unsigned)got, esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        char msg[96]; snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return send_json(req, msg);
    }
    ESP_LOGI(TAG, "Firmware written to %s in %lld ms; rebooting into it", target->label, (long long)((esp_timer_get_time() - t0) / 1000));
    reboot_soon(1500);
    return send_json(req, "{\"ok\":true,\"note\":\"written, rebooting\"}");
}

// -------------------------------------------------------------------- start
esp_err_t web_ui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 15; // seconds; firmware uploads over a weak link
    cfg.stack_size = 6144;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "HTTP server: %s", esp_err_to_name(err)); return err; }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/cmd", .method = HTTP_POST, .handler = cmd_post},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) httpd_register_uri_handler(server, &routes[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    // Captive-portal probes produce many odd requests: keep the log quiet.
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Control page on port 80");
    return ESP_OK;
}
