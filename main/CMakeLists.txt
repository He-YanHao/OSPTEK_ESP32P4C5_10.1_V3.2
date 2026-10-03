idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES spi_flash esp_psram lvgl_init guider_ui
)