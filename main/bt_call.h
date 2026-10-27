#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// HFP Hands-Free unit: the phone (Audio Gateway) routes its calls to us.
typedef enum { CALL_IDLE, CALL_INCOMING, CALL_OUTGOING, CALL_ACTIVE } call_state_t;

esp_err_t bt_call_init(void);
bool bt_call_connected(void); // service level connection up
call_state_t bt_call_state(void);
esp_err_t bt_call_connect(const uint8_t *bda);
void bt_call_disconnect(void);
void bt_call_answer(void);
void bt_call_hangup(void);  // reject an incoming call or end the current one
void bt_call_redial(void);  // last dialled number
void bt_call_dial(const char *number);
void bt_call_volume_step(int delta); // speaker gain 0..15, synced with the phone
void bt_call_log_status(void);
