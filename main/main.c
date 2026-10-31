// wifiblu: LyraT v4.3 Bluetooth speaker that also takes phone calls, with a Wi-Fi control page and OTA.
// Boot order matters: radio first (RF calibration isolated from codec/I2S
// activity, more initial heap), then codec + I2S, touch pads, Bluetooth profiles.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "board_config.h"
#include "app_keys.h"
#include "audio_engine.h"
#include "bt_core.h"
#include "bt_music.h"
#include "bt_call.h"
#include "touch_buttons.h"
#include "wifi_link.h"
#include "web_ui.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

// Touch pads: a call always wins over music.
void app_key(app_key_t key)
{
    call_state_t call = bt_call_state();
    switch (key) {
    case KEY_PLAY:
        if (call == CALL_INCOMING) bt_call_answer();
        else if (call != CALL_IDLE) bt_call_hangup();
        else bt_music_key(KEY_PLAY);
        break;
    case KEY_NEXT:
        if (call == CALL_IDLE) bt_music_key(KEY_NEXT);
        break;
    case KEY_DISCONNECT:
        if (call != CALL_IDLE) bt_call_hangup();
        else bt_core_disconnect();
        break;
    case KEY_VOL_UP:
    case KEY_VOL_DOWN:
        if (audio_engine_call_active()) bt_call_volume_step(key == KEY_VOL_UP ? 1 : -1);
        else bt_music_key(key);
        break;
    case KEY_WIFI_SETUP:
        wifi_link_start_setup();
        break;
    }
}

static void console_task(void *arg)
{
    ESP_LOGI("console", "Keys: a answer, h hangup/reject, m redial, c connect bonded phone, x disconnect, v list bonds, f forget bonds, "
                        "i status, t ring tone test, l/r/z/b tones, ]/[ DAC +-1.5 dB, 0 reset DAC + self-test, k power self-test, "
                        "w Wi-Fi status, W Wi-Fi setup hotspot, n forget Wi-Fi network");
    bool ring_test = false;
    while (1) {
        int c = getchar();
        switch (c) {
        case 'a': bt_call_answer(); break;
        case 'h': bt_call_hangup(); break;
        case 'm': bt_call_redial(); break;
        case 'c': bt_core_connect_last(); break;
        case 'x': bt_core_disconnect(); break;
        case 'v': bt_core_list_bonds(); break;
        case 'f': bt_core_forget_all(); break;
        case 'i': bt_core_log_status(); bt_call_log_status(); audio_engine_log_status(); wifi_link_log_status(); break;
        case 't': ring_test = !ring_test; audio_engine_ring(ring_test); break;
        case 'l': audio_engine_test_mode(1); break;
        case 'r': audio_engine_test_mode(2); break;
        case 'z': audio_engine_test_mode(3); break;
        case 'b': audio_engine_test_mode(0); break;
        case ']': audio_engine_dac_step(1); break;
        case '[': audio_engine_dac_step(-1); break;
        case '0': audio_engine_dac_reset(); break;
        case 'k': audio_engine_calibrate(); break;
        case 'w': wifi_link_log_status(); break;
        case 'W': wifi_link_start_setup(); break;
        case 'n': wifi_link_forget(); break;
        case EOF: clearerr(stdin); break;
        default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// OTA rollback: an image that was just installed boots as "pending verify".
// It confirms itself only once the Bluetooth stack is up and OTA_VALID_AFTER_S
// have passed without a reset; otherwise the next reset boots the old image.
static void ota_confirm_cb(void *arg)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(run, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (bt_core_ready()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI("boot", "New firmware in %s confirmed (Bluetooth ready); rollback cancelled", run->label);
        } else {
            ESP_LOGW("boot", "New firmware in %s not confirmed: Bluetooth not ready. Reset will roll back", run->label);
        }
    }
}

void app_main(void)
{
    static const char *reasons[] = {"unknown", "power-on / EN pin", "external", "software", "panic", "interrupt WDT",
                                    "task WDT", "other WDT", "deep sleep", "BROWNOUT", "SDIO", "USB", "JTAG", "efuse", "POWER GLITCH", "CPU lockup"};
    int why = esp_reset_reason();
    ESP_LOGI("boot", "wifiblu: music + calls (+ Wi-Fi planned); reset reason=%d (%s)", why,
             why >= 0 && why < (int)(sizeof(reasons) / sizeof(reasons[0])) ? reasons[why] : "?");
    gpio_config_t out = {.pin_bit_mask = (1ULL << BOARD_LED) | (1ULL << BOARD_PA_ENABLE), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(BOARD_LED, 0);
    gpio_set_level(BOARD_PA_ENABLE, 0);
    esp_err_t err = nvs_flash_init(); // also stores Bluetooth bonds
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(bt_core_radio_init());
    ESP_ERROR_CHECK(audio_engine_init());
    ESP_ERROR_CHECK(touch_buttons_init());
    ESP_ERROR_CHECK(bt_core_start());
    ESP_ERROR_CHECK(wifi_link_init()); // after Bluetooth: coexistence takes the radio in turns
    ESP_LOGI("boot", "Heap before web server: internal free=%u bytes (largest block %u), PSRAM free=%u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // The control page is optional: if it cannot start, the speaker still works over Bluetooth.
    if (web_ui_start() != ESP_OK) ESP_LOGE("boot", "Control page not started; Bluetooth audio continues");
    esp_timer_handle_t confirm;
    esp_timer_create_args_t args = {.callback = ota_confirm_cb, .name = "ota_confirm"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &confirm));
    ESP_ERROR_CHECK(esp_timer_start_once(confirm, OTA_VALID_AFTER_S * 1000000ULL));
    xTaskCreate(console_task, "console", 3584, NULL, 2, NULL);
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI("boot", "Initialized; running from %s; free heap=%lu bytes (internal %u)", run ? run->label : "?", (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
