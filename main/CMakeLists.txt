# MINIMAL_BUILD ON: only main and its REQUIRES chain get compiled.
# In CAMERA_RAW mode the LVGL / adapter / GUI Guider chain is not linked at all.
#
# MINIMAL_BUILD runs a component-discovery pass *before* Kconfig values are
# exported as CMake variables, so `if(CONFIG_APP_MODE_CAMERA_RAW)` alone would
# pick the GUI branch on that first pass and pin the wrong dependency chain.
# Fall back to reading sdkconfig so both passes agree.
set(APP_MODE_CAMERA_RAW FALSE)
if(CONFIG_APP_MODE_CAMERA_RAW)
    set(APP_MODE_CAMERA_RAW TRUE)
else()
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig" _app_mode_raw
         REGEX "^CONFIG_APP_MODE_CAMERA_RAW=y")
    if(_app_mode_raw)
        set(APP_MODE_CAMERA_RAW TRUE)
    endif()
endif()

if(APP_MODE_CAMERA_RAW)
    set(APP_SRCS "main.c" "cam_sccb_probe.c")
    set(APP_REQUIRES spi_flash esp_psram esp_mm esp_lcd lcd_jd9366
                     esp_driver_i2c esp_driver_gpio camera_preview)
else()
    set(APP_SRCS "main.c")
    set(APP_REQUIRES spi_flash esp_psram lvgl_init guider_ui)
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${APP_REQUIRES}
)