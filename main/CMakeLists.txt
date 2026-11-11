# MINIMAL_BUILD ON: only main and its REQUIRES chain get compiled.
# CAMERA_RAW / NET mode do not link the LVGL / adapter / GUI Guider chain.
#
# MINIMAL_BUILD runs a component-discovery pass *before* Kconfig values are
# exported as CMake variables, so `if(CONFIG_APP_MODE_*)` alone would pick
# the GUI branch on that first pass and pin the wrong dependency chain.
# Fall back to reading sdkconfig so both passes agree.
set(APP_MODE_CAMERA_RAW FALSE)
set(APP_MODE_NET FALSE)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig" _app_mode_lines
     REGEX "^CONFIG_APP_MODE_(CAMERA_RAW|NET)=y")
foreach(_line IN LISTS _app_mode_lines)
    if(_line MATCHES "^CONFIG_APP_MODE_CAMERA_RAW=y")
        set(APP_MODE_CAMERA_RAW TRUE)
    elseif(_line MATCHES "^CONFIG_APP_MODE_NET=y")
        set(APP_MODE_NET TRUE)
    endif()
endforeach()

if(CONFIG_APP_MODE_CAMERA_RAW)
    set(APP_MODE_CAMERA_RAW TRUE)
endif()
if(CONFIG_APP_MODE_NET)
    set(APP_MODE_NET TRUE)
endif()

if(APP_MODE_CAMERA_RAW)
    set(APP_SRCS "main.c" "cam_sccb_probe.c")
    set(APP_REQUIRES spi_flash esp_psram esp_mm esp_lcd lcd_jd9366
                     esp_driver_i2c esp_driver_gpio camera_preview)
elseif(APP_MODE_NET)
    # ESP-Hosted auto-initialises before app_main (Kconfig:
    # CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN). main only needs to
    # bring up the standard Wi-Fi station / netif on top of the SDIO NIC.
    set(APP_SRCS "main.c")
    set(APP_REQUIRES spi_flash esp_psram esp_hosted esp_wifi
                     esp_event esp_netif nvs_flash esp_driver_gpio)
else()
    set(APP_SRCS "main.c")
    set(APP_REQUIRES spi_flash esp_psram lvgl_init guider_ui)
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${APP_REQUIRES}
)
