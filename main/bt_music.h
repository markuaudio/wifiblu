#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_keys.h"

// A2DP sink (music from the phone) + AVRCP (play/pause, next, absolute volume).
esp_err_t bt_music_init(void);
bool bt_music_connected(void);
esp_err_t bt_music_connect(const uint8_t *bda);
void bt_music_disconnect(void);
void bt_music_key(app_key_t key); // KEY_PLAY, KEY_NEXT, KEY_VOL_UP, KEY_VOL_DOWN
