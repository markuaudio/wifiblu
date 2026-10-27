#pragma once
#include "esp_err.h"
// LyraT touch pads -> app_key(): PLAY tap, SET tap / 2 s hold, VOL+ / VOL- tap and auto-repeat.
esp_err_t touch_buttons_init(void);
