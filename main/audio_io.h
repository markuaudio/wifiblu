#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// I2S0 full duplex on the LyraT: TX (speaker) and RX (microphones) share
// MCLK/BCLK/LRCK. 32-bit slots, stereo; PCM16 is expanded/truncated here.
esp_err_t audio_io_init(unsigned rate);
esp_err_t audio_io_set_rate(unsigned rate, bool capture); // disables, reclocks, preloads silence, re-enables
unsigned audio_io_rate(void);
bool audio_io_write(const int16_t *stereo, size_t frames); // up to AUDIO_IO_MAX_FRAMES
bool audio_io_read(int16_t *stereo, size_t frames);        // blocks until frames arrived (capture on)
void audio_io_log_clocks(void);
#define AUDIO_IO_MAX_FRAMES 256
