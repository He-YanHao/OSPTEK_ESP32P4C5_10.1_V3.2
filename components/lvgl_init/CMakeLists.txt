idf_component_register(
    SRCS "lvgl_init.c"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl__lvgl
        lcd_jd9366
        touch_jd9366
        espressif__esp_lvgl_adapter
)