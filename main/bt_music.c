#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "audio_engine.h"
#include "bt_core.h"
#include "bt_music.h"

static const char *TAG = "music";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool connected, controller_connected, playing, volume_subscription;
static esp_bd_addr_t peer;
static uint8_t label;

bool bt_music_connected(void)
{
    portENTER_CRITICAL(&lock); bool c = connected; portEXIT_CRITICAL(&lock);
    return c;
}

esp_err_t bt_music_connect(const uint8_t *bda)
{
    esp_bd_addr_t remote; memcpy(remote, bda, sizeof(remote));
    return esp_a2d_sink_connect(remote);
}

void bt_music_disconnect(void)
{
    portENTER_CRITICAL(&lock);
    bool link = connected; esp_bd_addr_t remote; memcpy(remote, peer, sizeof(remote));
    portEXIT_CRITICAL(&lock);
    if (!link) return;
    esp_err_t err = esp_a2d_sink_disconnect(remote);
    if (err != ESP_OK) ESP_LOGW(TAG, "Disconnect: %s", esp_err_to_name(err));
}

static void volume_notify(bool interim)
{
    portENTER_CRITICAL(&lock);
    bool send = volume_subscription;
    if (!interim) volume_subscription = false;
    portEXIT_CRITICAL(&lock);
    if (send) {
        esp_avrc_rn_param_t value = {.volume = audio_engine_music_get_volume() * 127 / 100};
        esp_err_t err = esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
            interim ? ESP_AVRC_RN_RSP_INTERIM : ESP_AVRC_RN_RSP_CHANGED, &value);
        if (err != ESP_OK) ESP_LOGW(TAG, "Volume notification: %s", esp_err_to_name(err));
    }
}

void bt_music_key(app_key_t key)
{
    if (key == KEY_VOL_UP || key == KEY_VOL_DOWN) {
        int v = audio_engine_music_get_volume() + (key == KEY_VOL_UP ? 5 : -5);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_engine_music_set_volume(v); volume_notify(false); return;
    }
    portENTER_CRITICAL(&lock);
    bool link = connected, ct = controller_connected, is_playing = playing;
    portEXIT_CRITICAL(&lock);
    if (!link) { ESP_LOGI(TAG, "No phone connected for music"); return; }
    if (!ct) { ESP_LOGW(TAG, "Phone AVRCP control not connected yet"); return; }
    uint8_t command = key == KEY_NEXT ? ESP_AVRC_PT_CMD_FORWARD :
                      (is_playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
    ESP_LOGI(TAG, "Touch command: %s", key == KEY_NEXT ? "NEXT" : (is_playing ? "PAUSE" : "PLAY"));
    esp_err_t err = esp_avrc_ct_send_passthrough_cmd(label++ & 15, command, ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (err != ESP_OK) { ESP_LOGW(TAG, "Command rejected: %s", esp_err_to_name(err)); return; }
    vTaskDelay(pdMS_TO_TICKS(40)); // touch task only; never block a BT callback
    err = esp_avrc_ct_send_passthrough_cmd(label++ & 15, command, ESP_AVRC_PT_CMD_STATE_RELEASED);
    if (err != ESP_OK) ESP_LOGW(TAG, "Release rejected: %s", esp_err_to_name(err));
}

static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p)
{
    switch (event) {
    case ESP_A2D_PROF_STATE_EVT:
        if (p->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) bt_core_profile_ready(PROFILE_A2DP);
        break;
    case ESP_A2D_CONNECTION_STATE_EVT: {
        bool up = p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        if (up || p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            portENTER_CRITICAL(&lock);
            bool was = connected;
            connected = up; playing = false; memcpy(peer, p->conn_stat.remote_bda, sizeof(peer));
            if (!up) { controller_connected = false; volume_subscription = false; }
            portEXIT_CRITICAL(&lock);
            audio_engine_music_stream(false);
            // A failed connect attempt also ends in DISCONNECTED; that is not a lost link.
            if (up || was) bt_core_profile_link(PROFILE_A2DP, up, p->conn_stat.remote_bda);
            else bt_core_profile_failed(PROFILE_A2DP);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(TAG, "A2DP connecting...");
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_cie_sbc_t *sbc = &p->audio_cfg.mcc.cie.sbc_info;
        if (p->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) {
            audio_engine_music_stream(false); ESP_LOGE(TAG, "Unsupported codec; SBC required"); break;
        }
        unsigned rate = (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_48K) ? 48000 :
                        (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_44K) ? 44100 :
                        (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_32K) ? 32000 : 16000;
        audio_engine_music_format(rate, sbc->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_MONO ? 1 : 2);
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        bool started = p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        portENTER_CRITICAL(&lock); playing = started; portEXIT_CRITICAL(&lock);
        audio_engine_music_stream(started);
        ESP_LOGI(TAG, "Music %s", started ? "STARTED" : "SUSPENDED");
        break;
    }
    default: break;
    }
}

static void controller_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        portENTER_CRITICAL(&lock); controller_connected = p->conn_stat.connected; portEXIT_CRITICAL(&lock);
        if (p->conn_stat.connected) {
            esp_avrc_ct_send_get_play_status_cmd(0);
            esp_avrc_ct_send_get_rn_capabilities_cmd(1);
        }
        break;
    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        portENTER_CRITICAL(&lock); playing = p->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_PLAYING; portEXIT_CRITICAL(&lock);
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &p->get_rn_caps_rsp.evt_set,
                                              ESP_AVRC_RN_PLAY_STATUS_CHANGE))
            esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (p->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            portENTER_CRITICAL(&lock);
            playing = p->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING;
            portEXIT_CRITICAL(&lock);
            esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        }
        break;
    default: break;
    }
}

static void target_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *p)
{
    if (event == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        audio_engine_music_set_volume((p->set_abs_vol.volume * 100 + 63) / 127);
    } else if (event == ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT && p->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        portENTER_CRITICAL(&lock); volume_subscription = true; portEXIT_CRITICAL(&lock);
        volume_notify(true);
    } else if (event == ESP_AVRC_TG_CONNECTION_STATE_EVT && !p->conn_stat.connected) {
        portENTER_CRITICAL(&lock); volume_subscription = false; portEXIT_CRITICAL(&lock);
    }
}

esp_err_t bt_music_init(void)
{
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(controller_callback));
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(target_callback));
    esp_avrc_rn_evt_cap_mask_t events = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &events, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&events));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_callback));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(audio_engine_music_push));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    return ESP_OK;
}
