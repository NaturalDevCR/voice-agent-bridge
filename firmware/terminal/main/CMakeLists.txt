idf_component_register(
    SRCS "terminal.c" "audio.c" "config.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_gpio console esp_psram esp_timer
             esp_wifi esp_netif esp_event nvs_flash esp_app_format esp_http_client esp_lcd esp_driver_spi
)
