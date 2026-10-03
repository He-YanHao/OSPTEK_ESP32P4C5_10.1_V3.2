idf_component_register(
    SRCS "esp_lcd_jd9366.c" "lcd_jd9366.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_ledc
)