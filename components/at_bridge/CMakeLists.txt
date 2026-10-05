idf_component_register(
    SRCS "hal_uart.c" "hal_spi.c" "esp_at_spi.c" "app_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_uart esp_driver_spi freertos
)