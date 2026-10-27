#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"
#include "esp_log.h"
#include "board_config.h"
#include "audio_engine.h"
#include "bt_core.h"
#include "bt_call.h"

static const char *TAG = "call";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool slc, audio, in_progress, inband_ring;
    esp_hf_call_setup_status_t setup;
    esp_bd_addr_t peer;
    char number[32];
} s;

static const char *audio_state_str[] = {"disconnected", "connecting", "connected (CVSD 8 kHz)", "connected (mSBC 16 kHz)"};
static const char *setup_str[] = {"none", "INCOMING", "outgoing dialing", "outgoing alerting"};

bool bt_call_connected(void)
{
    portENTER_CRITICAL(&lock); bool c = s.slc; portEXIT_CRITICAL(&lock);
    return c;
}

call_state_t bt_call_state(void)
{
    portENTER_CRITICAL(&lock);
    bool active = s.in_progress; esp_hf_call_setup_status_t setup = s.setup;
    portEXIT_CRITICAL(&lock);
    if (setup == ESP_HF_CALL_SETUP_STATUS_INCOMING) return CALL_INCOMING;
    if (setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING || setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING) return CALL_OUTGOING;
    return active ? CALL_ACTIVE : CALL_IDLE;
}

esp_err_t bt_call_connect(const uint8_t *bda)
{
    esp_bd_addr_t remote; memcpy(remote, bda, sizeof(remote));
    return esp_hf_client_connect(remote);
}

void bt_call_disconnect(void)
{
    portENTER_CRITICAL(&lock);
    bool link = s.slc; esp_bd_addr_t remote; memcpy(remote, s.peer, sizeof(remote));
    portEXIT_CRITICAL(&lock);
    if (!link) return;
    esp_err_t err = esp_hf_client_disconnect(remote);
    if (err != ESP_OK) ESP_LOGW(TAG, "Disconnect: %s", esp_err_to_name(err));
}

static void update_ring(void)
{
    // Ring locally only while an incoming call waits and the phone does not
    // stream its own ring tone (in-band ring arrives over the voice link).
    portENTER_CRITICAL(&lock);
    bool ring = s.slc && s.setup == ESP_HF_CALL_SETUP_STATUS_INCOMING && !s.audio;
    portEXIT_CRITICAL(&lock);
    audio_engine_ring(ring);
}

void bt_call_answer(void)
{
    if (bt_call_state() != CALL_INCOMING) { ESP_LOGI(TAG, "No incoming call to answer"); return; }
    ESP_LOGI(TAG, "Answering call%s%s", s.number[0] ? " from " : "", s.number);
    esp_err_t err = esp_hf_client_answer_call();
    if (err != ESP_OK) ESP_LOGW(TAG, "Answer: %s", esp_err_to_name(err));
}

void bt_call_hangup(void)
{
    call_state_t state = bt_call_state();
    if (state == CALL_IDLE) { ESP_LOGI(TAG, "No call to end"); return; }
    ESP_LOGI(TAG, "%s call", state == CALL_INCOMING ? "Rejecting" : "Ending");
    esp_err_t err = esp_hf_client_reject_call(); // AT+CHUP: rejects or hangs up
    if (err != ESP_OK) ESP_LOGW(TAG, "Hang up: %s", esp_err_to_name(err));
}

void bt_call_redial(void)
{
    if (!bt_call_connected()) { ESP_LOGI(TAG, "Phone not connected for calls"); return; }
    ESP_LOGI(TAG, "Redialling last number");
    esp_err_t err = esp_hf_client_dial(NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "Redial: %s", esp_err_to_name(err));
}

void bt_call_dial(const char *number)
{
    if (!bt_call_connected()) { ESP_LOGI(TAG, "Phone not connected for calls"); return; }
    ESP_LOGI(TAG, "Dialling %s", number);
    esp_err_t err = esp_hf_client_dial(number);
    if (err != ESP_OK) ESP_LOGW(TAG, "Dial: %s", esp_err_to_name(err));
}

void bt_call_volume_step(int delta)
{
    int v = (int)audio_engine_call_get_volume() + delta;
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    audio_engine_call_set_volume(v);
    if (bt_call_connected()) esp_hf_client_volume_update(ESP_HF_VOLUME_CONTROL_TARGET_SPK, v);
}

void bt_call_log_status(void)
{
    portENTER_CRITICAL(&lock);
    bool slc = s.slc, audio = s.audio, prog = s.in_progress, inband = s.inband_ring; esp_hf_call_setup_status_t setup = s.setup;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Status: link %s, voice %s, call %s, setup %s, in-band ring %s, number '%s'",
             slc ? "UP" : "down", audio ? "UP" : "down", prog ? "in progress" : "none",
             setup_str[setup <= 3 ? setup : 0], inband ? "yes" : "no", s.number);
}

static void incoming_pcm(const uint8_t *buf, uint32_t len) { audio_engine_call_push(buf, len); }
static uint32_t outgoing_pcm(uint8_t *buf, uint32_t len) { return audio_engine_call_pull(buf, len); }

static void hf_callback(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *p)
{
    switch (event) {
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        if (p->prof_stat.state == ESP_HF_INIT_SUCCESS) bt_core_profile_ready(PROFILE_HFP);
        else if (p->prof_stat.state != ESP_HF_DEINIT_SUCCESS) ESP_LOGE(TAG, "HFP init state %d", p->prof_stat.state);
        break;
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        esp_hf_client_connection_state_t st = p->conn_stat.state;
        ESP_LOGI(TAG, "Link state %d (3 = ready for calls), phone features 0x%"PRIx32, st, p->conn_stat.peer_feat);
        if (st == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            portENTER_CRITICAL(&lock);
            s.slc = true; memcpy(s.peer, p->conn_stat.remote_bda, sizeof(s.peer));
            portEXIT_CRITICAL(&lock);
            bt_core_profile_link(PROFILE_HFP, true, p->conn_stat.remote_bda);
            esp_hf_client_volume_update(ESP_HF_VOLUME_CONTROL_TARGET_SPK, audio_engine_call_get_volume());
        } else if (st == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            bool was;
            portENTER_CRITICAL(&lock);
            was = s.slc; s.slc = false; s.audio = false; s.in_progress = false; s.setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
            s.number[0] = 0;
            portEXIT_CRITICAL(&lock);
            audio_engine_ring(false);
            if (audio_engine_call_active()) audio_engine_call_stop();
            if (was) bt_core_profile_link(PROFILE_HFP, false, p->conn_stat.remote_bda);
            else {
                ESP_LOGW(TAG, "HFP link attempt ended before service level connection (phone off, out of range, or no hands-free support)");
                bt_core_profile_failed(PROFILE_HFP);
            }
        }
        break;
    }
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        esp_hf_client_audio_state_t st = p->audio_stat.state;
        ESP_LOGI(TAG, "Voice link %s", audio_state_str[st <= 3 ? st : 0]);
        if (st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED || st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            portENTER_CRITICAL(&lock); s.audio = true; portEXIT_CRITICAL(&lock);
            esp_hf_client_register_data_callback(incoming_pcm, outgoing_pcm);
            audio_engine_call_start(st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? 16000 : 8000);
        } else if (st == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            portENTER_CRITICAL(&lock); s.audio = false; portEXIT_CRITICAL(&lock);
            audio_engine_call_stop();
            update_ring();
        }
        break;
    }
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        portENTER_CRITICAL(&lock); s.in_progress = p->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS; portEXIT_CRITICAL(&lock);
        ESP_LOGI(TAG, "Call %s", s.in_progress ? "ACTIVE" : "ended / none");
        if (!s.in_progress) s.number[0] = 0;
        update_ring();
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        portENTER_CRITICAL(&lock); s.setup = p->call_setup.status; portEXIT_CRITICAL(&lock);
        ESP_LOGI(TAG, "Call setup: %s%s%s", setup_str[s.setup <= 3 ? s.setup : 0],
                 s.setup == ESP_HF_CALL_SETUP_STATUS_INCOMING ? " -> PLAY = answer, SET hold = reject" : "", "");
        update_ring();
        break;
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "RING%s%s", s.number[0] ? " from " : "", s.number);
        update_ring();
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        if (p->clip.number) { strncpy(s.number, p->clip.number, sizeof(s.number) - 1); s.number[sizeof(s.number) - 1] = 0; }
        ESP_LOGI(TAG, "Caller number: %s", p->clip.number ? p->clip.number : "(hidden)");
        break;
    case ESP_HF_CLIENT_CCWA_EVT:
        ESP_LOGI(TAG, "Call waiting from %s", p->ccwa.number ? p->ccwa.number : "(hidden)");
        break;
    case ESP_HF_CLIENT_BSIR_EVT:
        portENTER_CRITICAL(&lock); s.inband_ring = p->bsir.state == ESP_HF_CLIENT_IN_BAND_RINGTONE_PROVIDED; portEXIT_CRITICAL(&lock);
        ESP_LOGI(TAG, "Phone %s its own ring tone over the voice link", s.inband_ring ? "sends" : "does not send");
        break;
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        if (p->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK) audio_engine_call_set_volume(p->volume_control.volume);
        else ESP_LOGI(TAG, "Phone set mic gain %d (fixed PGA here)", p->volume_control.volume);
        break;
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        ESP_LOGI(TAG, "Call held status %d", p->call_held.status);
        break;
    case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
        ESP_LOGI(TAG, "Phone network %s", p->service_availability.status ? "available" : "NOT available");
        break;
    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
        ESP_LOGI(TAG, "Phone signal %d/5", p->signal_strength.value);
        break;
    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
        ESP_LOGI(TAG, "Phone battery %d/5", p->battery_level.value);
        break;
    case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
        ESP_LOGI(TAG, "Roaming %s", p->roaming.status ? "active" : "inactive");
        break;
    case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
        ESP_LOGI(TAG, "Operator: %s", p->cops.name ? p->cops.name : "?");
        break;
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        if (p->at_response.code != ESP_HF_AT_RESPONSE_CODE_OK)
            ESP_LOGW(TAG, "Phone rejected a command: code %d, cme %d", p->at_response.code, p->at_response.cme);
        break;
    case ESP_HF_CLIENT_CLCC_EVT:
        ESP_LOGI(TAG, "Current call %d: dir %d, state %d, number %s", p->clcc.idx, p->clcc.dir, p->clcc.status,
                 p->clcc.number ? p->clcc.number : "?");
        break;
    default:
        ESP_LOGD(TAG, "HFP event %d", event);
        break;
    }
}

esp_err_t bt_call_init(void)
{
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_callback));
    ESP_ERROR_CHECK(esp_hf_client_init());
    return ESP_OK;
}
