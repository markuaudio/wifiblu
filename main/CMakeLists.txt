idf_component_register(SRCS "main.c" "es8388.c" "audio_io.c" "audio_engine.c"
                            "bt_core.c" "bt_music.c" "bt_call.c" "touch_buttons.c" "power_guard.c"
                            "wifi_link.c" "web_ui.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html"
    REQUIRES bt nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_touch_sens esp_timer esp_ringbuf esp_hw_support
             esp_wifi esp_netif esp_event esp_http_server app_update esp_app_format dns_server)
