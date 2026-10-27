idf_component_register(SRCS "main.c" "es8388.c" "audio_io.c" "audio_engine.c"
                            "bt_core.c" "bt_music.c" "bt_call.c" "touch_buttons.c" "power_guard.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_touch_sens esp_timer esp_ringbuf esp_hw_support)
