idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES spi_flash esp_psram lcd_jd9366 touch_jd9366
)