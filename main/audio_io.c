#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "board_config.h"
#include "audio_io.h"

static const char *TAG = "i2s";
static i2s_chan_handle_t tx, rx;
static unsigned current_rate;
static bool capturing;

static i2s_std_clk_config_t clock_config(unsigned rate, bool for_rx)
{
    (void)for_rx;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.clk_src = I2S_CLK_SRC_DEFAULT;
    // MCLK = 256 Fs, matching the ES8388 ratio registers (0x18/0x0d = 0x02).
    // On the original ESP32 TX and RX share one MCLK divider, so both channels
    // must ask for the same multiple. The RX channel becomes an internal duplex
    // slave and needs MCLK/BCLK >= 4: 256 / (2 slots x 32 bits) = 4 exactly.
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return clk;
}

static void preload_silence(void)
{
    int32_t silence[AUDIO_IO_MAX_FRAMES * 2] = {0};
    for (int i = 0; i < 8; ++i) {
        size_t loaded = 0;
        ESP_ERROR_CHECK(i2s_channel_preload_data(tx, silence, sizeof(silence), &loaded));
        if (!loaded) break;
    }
}

esp_err_t audio_io_init(unsigned rate)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8; channel.dma_frame_num = AUDIO_IO_MAX_FRAMES;
    channel.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &tx, &rx));
    i2s_std_config_t cfg = {
        .clk_cfg = clock_config(rate, false),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = BOARD_I2S_MCLK, .bclk = BOARD_I2S_BCLK,
                     .ws = BOARD_I2S_LRCK, .dout = BOARD_I2S_DOUT, .din = I2S_GPIO_UNUSED},
    };
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT; // 64 BCLK per stereo frame (ES8388 framing)
    cfg.slot_cfg.ws_width = 32;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &cfg)); // master first
    cfg.clk_cfg = clock_config(rate, true);
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = BOARD_I2S_DIN;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &cfg)); // becomes the duplex slave on the same pins
    current_rate = rate;
    preload_silence();
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    audio_io_log_clocks();
    return ESP_OK;
}

unsigned audio_io_rate(void) { return current_rate; }

esp_err_t audio_io_set_rate(unsigned rate, bool capture)
{
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
    if (capturing) ESP_ERROR_CHECK(i2s_channel_disable(rx));
    if (rate != current_rate) {
        i2s_std_clk_config_t clk = clock_config(rate, false);
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx, &clk));
        clk = clock_config(rate, true);
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(rx, &clk));
        current_rate = rate;
    }
    preload_silence();
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    if (capture) ESP_ERROR_CHECK(i2s_channel_enable(rx));
    capturing = capture;
    audio_io_log_clocks();
    return ESP_OK;
}

bool audio_io_write(const int16_t *stereo, size_t frames)
{
    // Original ESP32 FIFO selects packing from slot width: data and slot widths
    // must agree, so expand signed PCM16 to MSB-aligned PCM32 here.
    if (frames > AUDIO_IO_MAX_FRAMES) return false;
    int32_t wire[AUDIO_IO_MAX_FRAMES * 2];
    for (size_t i = 0; i < frames * 2; ++i) wire[i] = (int32_t)stereo[i] * 65536;
    size_t total = frames * 2 * sizeof(int32_t), written = 0;
    const uint8_t *p = (const uint8_t *)wire;
    while (written < total) {
        size_t n = 0;
        if (i2s_channel_write(tx, p + written, total - written, &n, 200) != ESP_OK || !n) return false;
        written += n;
    }
    return true;
}

bool audio_io_read(int16_t *stereo, size_t frames)
{
    if (!capturing || frames > AUDIO_IO_MAX_FRAMES) return false;
    int32_t wire[AUDIO_IO_MAX_FRAMES * 2];
    size_t total = frames * 2 * sizeof(int32_t), got = 0;
    uint8_t *p = (uint8_t *)wire;
    while (got < total) {
        size_t n = 0;
        if (i2s_channel_read(rx, p + got, total - got, &n, 200) != ESP_OK || !n) return false;
        got += n;
    }
    for (size_t i = 0; i < frames * 2; ++i) stereo[i] = (int16_t)(wire[i] >> 16);
    return true;
}

void audio_io_log_clocks(void)
{
    i2s_chan_info_t info;
    ESP_ERROR_CHECK(i2s_channel_get_info(tx, &info));
    ESP_LOGI(TAG, "TX clocks: source=%"PRIu32" MCLK=%"PRIu32" BCLK=%"PRIu32" Fs=%u capture=%d (32-bit data and slots)",
             info.sclk_hz, info.mclk_hz, info.bclk_hz, current_rate, capturing);
}
