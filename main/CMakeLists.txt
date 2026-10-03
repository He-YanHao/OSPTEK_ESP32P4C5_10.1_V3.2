idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES spi_flash esp_psram esp_lcd_jd9366
)