idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "display_ui.c"
        "i2c_shared.c"
        "mqtt.c"
        "sensors.c"
        "sh1106.c"
        "vl53l4cd.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio esp_driver_i2c esp_lcd esp_timer
        esp_wifi esp_event esp_netif nvs_flash mqtt mbedtls
)
