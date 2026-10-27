#pragma once
// Physical controls (touch pads) and what they mean. main.c decides whether a
// key goes to the call (HFP) or to the music (A2DP/AVRCP) side.
typedef enum { KEY_PLAY, KEY_NEXT, KEY_DISCONNECT, KEY_VOL_UP, KEY_VOL_DOWN } app_key_t;
void app_key(app_key_t key);
