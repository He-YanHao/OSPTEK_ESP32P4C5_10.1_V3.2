idf_component_register(
    SRCS "touch_jd9366.c" "esp_lcd_touch_jd9366.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c esp_lcd esp_lcd_touch lcd_jd9366
)