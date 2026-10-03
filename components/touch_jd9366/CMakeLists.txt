idf_component_register(
    SRCS "touch_jd9366.c" "esp_lcd_touch.c" "esp_lcd_touch_jd9366.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver
)