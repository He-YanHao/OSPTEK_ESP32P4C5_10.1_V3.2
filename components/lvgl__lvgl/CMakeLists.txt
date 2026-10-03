cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE LVGL_SOURCES "src/*.c")

idf_component_register(
    SRCS ${LVGL_SOURCES}
    INCLUDE_DIRS "." "src"
    REQUIRES esp_timer freertos driver esp_driver_ppa esp_mm
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LV_LVGL_H_INCLUDE_SIMPLE
    LV_CONF_INCLUDE_SIMPLE
    CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE=64
    CONFIG_LV_DRAW_BUF_ALIGN=64
)