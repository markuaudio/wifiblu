// ES8388 register sequence follows Espressif's reference driver
// (docs/references/es8388-reference.c.txt) and the kanchan speaker project's
// verified playback settings (LOUT2/ROUT2 at -45 dB: they feed the NS4150
// inverting inputs on LyraT v4.3 and cancel the signal at 0 dB).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board_config.h"
#include "es8388.h"

static const char *TAG = "es8388";
static i2c_master_dev_handle_t codec;

// DAC digital volume staircase, 1.5 dB per step (reg 0x1a/0x1b, 0.5 dB units).
static const uint8_t dac_profiles[] = {
    0x18, // P0: -12.0 dB (safe start on a weak USB port)
    0x15, // P1: -10.5 dB
    0x12, // P2:  -9.0 dB (NS4150 clip point for 0 dBFS peaks, est.)
    0x0f, // P3:  -7.5 dB
    0x0c, // P4:  -6.0 dB (ESP-ADF LyraT maximum)
    0x09, // P5:  -4.5 dB
    0x06, // P6:  -3.0 dB
    0x03, // P7:  -1.5 dB
    0x00, // P8:   0.0 dB (register maximum; PA overdriven on peaks)
};

esp_err_t es8388_write(uint8_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(codec, bytes, sizeof(bytes), 100);
}

esp_err_t es8388_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(codec, &reg, 1, value, 1, 100);
}

unsigned es8388_dac_profile_max(void)
{
    return sizeof(dac_profiles) / sizeof(dac_profiles[0]) - 1;
}

float es8388_dac_profile_db(unsigned profile)
{
    if (profile > es8388_dac_profile_max()) profile = es8388_dac_profile_max();
    return -0.5f * dac_profiles[profile];
}

void es8388_dac_mute(bool mute)
{
    ESP_ERROR_CHECK(es8388_write(0x19, mute ? 0x04 : 0x00));
}

void es8388_apply_dac_profile(unsigned profile)
{
    if (profile > es8388_dac_profile_max()) profile = es8388_dac_profile_max();
    ESP_ERROR_CHECK(es8388_write(0x1a, dac_profiles[profile]));
    ESP_ERROR_CHECK(es8388_write(0x1b, dac_profiles[profile]));
    ESP_ERROR_CHECK(es8388_write(0x2e, 0x1e)); // LOUT1/ROUT1 0 dB -> PA IN+
    ESP_ERROR_CHECK(es8388_write(0x2f, 0x1e));
    ESP_ERROR_CHECK(es8388_write(0x30, 0x00)); // LOUT2/ROUT2 -45 dB -> PA IN- (must stay low)
    ESP_ERROR_CHECK(es8388_write(0x31, 0x00));
    ESP_LOGI(TAG, "DAC profile P%u: %.1f dB (reg 0x%02x)", profile, (double)es8388_dac_profile_db(profile), dac_profiles[profile]);
}

void es8388_adc_power(bool on)
{
    // 0x00: ADC L/R, analog inputs and MICBIAS all on. 0xff: everything off.
    ESP_ERROR_CHECK(es8388_write(0x03, on ? 0x00 : 0xff));
    ESP_LOGI(TAG, "ADC/microphones %s", on ? "ON" : "OFF");
}

esp_err_t es8388_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CODEC_ADDR, .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_probe(bus, BOARD_CODEC_ADDR, 200));
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev, &codec));

    const uint8_t init[][2] = {
        {0x19, 0x04}, // DAC mute
        {0x01, 0x50}, // analog/reference power configuration
        {0x02, 0x00}, // chip normal operation
        {0x35, 0xa0}, {0x37, 0xd0}, {0x39, 0xd0}, // reference DLL settings
        {0x08, 0x00}, // codec SLAVE; ESP32 supplies MCLK/BCLK/LRCK
        {0x04, 0xc0}, // DAC/output power down during setup
        {0x00, 0x36}, // SameFs=1, DACMCLK=1, EnRef=1, VMID=500k: ADC and DAC share one Fs
        {0x03, 0xff}, // ADC/mics OFF until a call starts
        // ---- record path (onboard microphones) ----
        {0x09, MIC_PGA_REG}, // mic PGA gain, both channels
        {0x0a, 0x00}, // LINSEL/RINSEL = LINPUT1/RINPUT1 (LyraT onboard mics, like ESP-ADF LINE1)
        {0x0b, 0x02}, // left data = left ADC, right data = right ADC
        {0x0c, 0x10}, // ADC 32-bit Philips I2S; matches the 32-bit slots on the shared bus
        {0x0d, 0x02}, // ADC MCLK / sample rate = 256
        {0x10, 0x00}, {0x11, 0x00}, // ADC digital volume 0 dB
        // ---- playback path ----
        {0x17, 0x20}, // DAC 32-bit Philips I2S
        {0x18, 0x02}, // DAC MCLK / sample rate = 256
        {0x26, 0x00}, // no analog input bypass
        {0x27, 0x90}, // left DAC -> left output mixer
        {0x2a, 0x90}, // right DAC -> right output mixer
        {0x2b, 0x80}, // shared LRCK, DAC LRCK source
        {0x2d, 0x00},
        {0x2e, 0x1e}, {0x2f, 0x1e}, // LOUT1 / ROUT1 -> NS4150 IN+ at 0 dB
        {0x30, 0x00}, {0x31, 0x00}, // LOUT2 / ROUT2 -> NS4150 IN- at -45 dB (cancellation fix)
        {0x1a, dac_profiles[AUDIO_BOOT_DAC_PROFILE]}, {0x1b, dac_profiles[AUDIO_BOOT_DAC_PROFILE]},
        {0x04, 0x3c}, // enable both DACs and both output pairs
        {0x02, 0xf0}, {0x02, 0x00}, // restart codec state machine
    };
    for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); ++i) {
        esp_err_t err = es8388_write(init[i][0], init[i][1]);
        if (err != ESP_OK) return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Ready: DAC P%u (%.1f dB) -> LOUT1 0 dB -> PA; mics LIN1/RIN1 PGA reg 0x%02x (ADC off until a call)",
             (unsigned)AUDIO_BOOT_DAC_PROFILE, (double)es8388_dac_profile_db(AUDIO_BOOT_DAC_PROFILE), MIC_PGA_REG);
    return ESP_OK;
}

void es8388_log_readback(void)
{
    const uint8_t regs[] = {0x00, 0x03, 0x04, 0x08, 0x09, 0x0a, 0x0c, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x2e, 0x2f, 0x30, 0x31};
    uint8_t v[sizeof(regs)] = {0};
    for (unsigned i = 0; i < sizeof(regs); ++i) ESP_ERROR_CHECK(es8388_read(regs[i], &v[i]));
    ESP_LOGI(TAG, "readback ctrl=%02x adcpwr=%02x dacpwr=%02x master=%02x pga=%02x insel=%02x adcfmt=%02x dacfmt=%02x ratio=%02x mute=%02x",
             v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    ESP_LOGI(TAG, "readback LDAC=%02x RDAC=%02x L1=%02x R1=%02x L2=%02x R2=%02x", v[10], v[11], v[12], v[13], v[14], v[15]);
}
