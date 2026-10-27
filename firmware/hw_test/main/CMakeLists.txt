idf_component_register(
    SRCS "hw_test.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_gpio console esp_psram esp_timer
)
