#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_config.h"
#include "app_keys.h"
#include "touch_buttons.h"

static touch_sensor_handle_t sensor;
static touch_channel_handle_t pads[4];
static const int ids[] = {BOARD_TOUCH_PLAY, BOARD_TOUCH_SET, BOARD_TOUCH_UP, BOARD_TOUCH_DOWN};
static const char *names[] = {"PLAY GPIO33", "SET GPIO32", "VOL+ GPIO27", "VOL- GPIO13"};
static float baseline[4];

static void touch_task(void *arg)
{
    bool pressed[4] = {0}, candidate[4] = {0}, long_sent[4] = {0};
    int64_t changed[4] = {0}, down[4] = {0}, repeated[4] = {0};
    ESP_LOGI("touch", "Ready: PLAY = answer / hang up / play-pause, SET = next (hold 2 s: reject / disconnect), VOL +/-");
    while (1) {
        int64_t now = esp_timer_get_time();
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t raw[1];
            if (touch_channel_read_data(pads[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, raw) != ESP_OK) continue;
            // ESP32 V1: capacitance increases -> measured count falls.
            bool active = raw[0] < baseline[i] * (pressed[i] ? 0.87f : 0.80f);
            if (!active && !pressed[i] && raw[0] > baseline[i] * 0.90f)
                baseline[i] += ((float)raw[0] - baseline[i]) * 0.001f;
            if (active != candidate[i]) { candidate[i] = active; changed[i] = now; }
            if (active != pressed[i] && now - changed[i] >= 60000) {
                pressed[i] = active;
                if (active) {
                    down[i] = repeated[i] = now; long_sent[i] = false;
                    ESP_LOGI("touch", "%s pressed (raw=%"PRIu32")", names[i], raw[0]);
                    if (i == 0) app_key(KEY_PLAY);
                    if (i == 2) app_key(KEY_VOL_UP);
                    if (i == 3) app_key(KEY_VOL_DOWN);
                } else if (i == 1 && !long_sent[i]) app_key(KEY_NEXT);
            }
            if (pressed[i] && i == 1 && !long_sent[i] && now - down[i] >= 2000000) {
                long_sent[i] = true; app_key(KEY_DISCONNECT);
            }
            if (pressed[i] && i >= 2 && now - down[i] >= 500000 && now - repeated[i] >= 200000) {
                repeated[i] = now; app_key(i == 2 ? KEY_VOL_UP : KEY_VOL_DOWN);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t touch_buttons_init(void)
{
    touch_sensor_sample_config_t sample[] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7)
    };
    touch_sensor_config_t cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&cfg, &sensor));
    touch_channel_config_t channel = {
        .abs_active_thresh = {1000}, .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT, .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
    for (int i = 0; i < 4; ++i) ESP_ERROR_CHECK(touch_sensor_new_channel(sensor, ids[i], &channel, &pads[i]));
    touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(sensor, &filter));
    ESP_ERROR_CHECK(touch_sensor_enable(sensor));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(sensor));
    ESP_LOGI("touch", "Calibration: do not touch pads for 1 second");
    vTaskDelay(pdMS_TO_TICKS(300));
    for (int n = 0; n < 32; ++n) {
        for (int i = 0; i < 4; ++i) {
            uint32_t raw[1];
            ESP_ERROR_CHECK(touch_channel_read_data(pads[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, raw));
            baseline[i] += raw[0] / 32.0f;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (int i = 0; i < 4; ++i) {
        if (baseline[i] < 1) return ESP_ERR_INVALID_RESPONSE;
        ESP_LOGI("touch", "%s baseline=%.0f", names[i], baseline[i]);
    }
    if (xTaskCreate(touch_task, "touch_keys", 4096, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
