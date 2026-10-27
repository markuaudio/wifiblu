#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ES8388 codec over I2C. Playback path (DAC -> LOUT1/ROUT1 -> NS4150 PA) and
// record path (onboard mics -> LINPUT1/RINPUT1 -> ADC) are both configured at
// init; the ADC stays powered down until a call needs the microphones.
esp_err_t es8388_init(void);
esp_err_t es8388_write(uint8_t reg, uint8_t value);
esp_err_t es8388_read(uint8_t reg, uint8_t *value);
void es8388_dac_mute(bool mute);
void es8388_apply_dac_profile(unsigned profile); // index into dac_profiles[]
unsigned es8388_dac_profile_max(void);
float es8388_dac_profile_db(unsigned profile);
void es8388_adc_power(bool on);                  // microphones + ADC
void es8388_log_readback(void);
