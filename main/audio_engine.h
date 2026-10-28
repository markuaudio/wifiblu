#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// One task owns the ES8388 + I2S bus and switches between three sources:
//   MUSIC  : A2DP PCM from the phone (stereo, 44.1/48 kHz)
//   CALL   : HFP voice PCM (mono 8/16 kHz) out to the speaker, microphones back to the phone
//   RING   : locally generated ring tone while an incoming call waits
esp_err_t audio_engine_init(void);

// Music (A2DP sink) side.
void audio_engine_music_format(unsigned rate, unsigned channels);
void audio_engine_music_stream(bool active);
void audio_engine_music_push(const uint8_t *data, uint32_t length); // Bluetooth callback: copy and return
void audio_engine_music_set_volume(unsigned percent);
unsigned audio_engine_music_get_volume(void);

// Call (HFP) side.
void audio_engine_call_start(unsigned rate); // SCO up: 8000 (CVSD) or 16000 (mSBC)
void audio_engine_call_stop(void);           // SCO down
bool audio_engine_call_active(void);
void audio_engine_call_push(const uint8_t *pcm, uint32_t length); // phone -> speaker
uint32_t audio_engine_call_pull(uint8_t *pcm, uint32_t length);   // microphones -> phone
void audio_engine_call_set_volume(unsigned level); // HFP speaker gain 0..15
unsigned audio_engine_call_get_volume(void);
void audio_engine_ring(bool on); // local ring tone (when the phone sends no in-band ring)

// Diagnostics (serial console).
void audio_engine_test_mode(unsigned mode); // 0=Bluetooth, 1=left 400 Hz, 2=right 800 Hz, 3=digital silence
void audio_engine_dac_step(int delta);      // ] / [ : 1.5 dB codec steps, not persisted
void audio_engine_dac_reset(void);      // 0 : default profile, forget the learned limit, run the power self-test again
void audio_engine_calibrate(void);      // k : run the power self-test now
void audio_engine_log_status(void);
unsigned audio_engine_dac_profile(void);   // current ES8388 DAC profile index (web page)
