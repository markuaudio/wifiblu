#include "esp_attr.h"
#include "esp_log.h"
#include "hal/brownout_hal.h"
#include "hal/brownout_ll.h"
#include "esp_private/rtc_ctrl.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/gpio_struct.h"
#include "board_config.h"
#include "power_guard.h"

static const char *TAG = "guard";
static volatile uint32_t s_events;
static volatile bool s_pending;

// Runs from the RTC interrupt with the cache possibly disabled: registers only.
static void IRAM_ATTR bod_isr(void *arg)
{
    GPIO.out_w1tc = 1u << BOARD_PA_ENABLE; // amplifier off: the load disappears within microseconds
    brownout_ll_intr_enable(false);        // no interrupt storm while the rail is still low
    brownout_ll_intr_clear();
    ++s_events;
    s_pending = true;
}

esp_err_t power_guard_init(void)
{
    brownout_hal_config_t cfg = {
        .threshold = POWER_GUARD_BOD_LEVEL, // 7 = 2.80 V on the ESP32, above the IMP809's 2.63 V
        .enabled = true,
        .reset_enabled = false,
        .flash_power_down = false,          // keep running: no reset, no flash/RF power-down
        .rf_power_down = false,
    };
    brownout_hal_config(&cfg);
    brownout_ll_intr_clear();
    esp_err_t err = rtc_isr_register(bod_isr, NULL, RTC_CNTL_BROWN_OUT_INT_ENA_M, RTC_INTR_FLAG_IRAM);
    if (err != ESP_OK) return err;
    brownout_ll_intr_enable(true);
    ESP_LOGI(TAG, "Brownout guard armed at level %d (2.80 V): a sagging rail cuts the amplifier instead of resetting", POWER_GUARD_BOD_LEVEL);
    return ESP_OK;
}

bool power_guard_take_event(void)
{
    if (!s_pending) return false;
    s_pending = false;
    return true;
}

uint32_t power_guard_events(void) { return s_events; }

void power_guard_rearm(void)
{
    brownout_ll_intr_clear();
    brownout_ll_intr_enable(true);
}
