# MINIMAL_BUILD ON: only main and its REQUIRES chain get compiled.
#
# MINIMAL_BUILD runs a component-discovery pass *before* Kconfig values are
# exported as CMake variables, so `if(CONFIG_APP_MODE_*)` alone would pick
# the GUI branch on that first pass and pin the wrong dependency chain.
# Fall back to reading sdkconfig so both passes agree.
set(APP_MODE_CAMERA_RAW FALSE)
set(APP_MODE_NET FALSE)
set(APP_MODE_H264 FALSE)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig" _app_mode_lines
     REGEX "^CONFIG_APP_MODE_(CAMERA_RAW|NET|H264)=y")
foreach(_line IN LISTS _app_mode_lines)
    if(_line MATCHES "^CONFIG_APP_MODE_CAMERA_RAW=y")
        set(APP_MODE_CAMERA_RAW TRUE)
    elseif(_line MATCHES "^CONFIG_APP_MODE_NET=y")
        set(APP_MODE_NET TRUE)
    elseif(_line MATCHES "^CONFIG_APP_MODE_H264=y")
        set(APP_MODE_H264 TRUE)
    endif()
endforeach()

if(CONFIG_APP_MODE_CAMERA_RAW)
    set(APP_MODE_CAMERA_RAW TRUE)
endif()
if(CONFIG_APP_MODE_NET)
    set(APP_MODE_NET TRUE)
endif()
if(CONFIG_APP_MODE_H264)
    set(APP_MODE_H264 TRUE)
endif()

# Common network chain: ESP-Hosted auto-initialises before app_main
# (CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN); main only brings up
# the standard Wi-Fi station / netif on top of the SDIO NIC.
set(APP_REQUIRES_NET esp_hosted esp_wifi esp_event esp_netif nvs_flash lwip)

if(APP_MODE_H264)
    # S3: camera preview (RGB565 -> DPI fb) + P4 HW H.264 encoder + network.
    set(APP_SRCS "main.c" "h264_sink.c" "rtsp_server.c")
    set(APP_REQUIRES spi_flash esp_psram esp_mm esp_lcd lcd_jd9366
                     esp_driver_i2c esp_driver_gpio camera_preview
                     espressif__esp_h264 ${APP_REQUIRES_NET})
elseif(APP_MODE_CAMERA_RAW)
    set(APP_SRCS "main.c" "cam_sccb_probe.c")
    set(APP_REQUIRES spi_flash esp_psram esp_mm esp_lcd lcd_jd9366
                     esp_driver_i2c esp_driver_gpio camera_preview)
elseif(APP_MODE_NET)
    set(APP_SRCS "main.c")
    set(APP_REQUIRES spi_flash esp_psram esp_driver_gpio ${APP_REQUIRES_NET})
else()
    set(APP_SRCS "main.c")
    set(APP_REQUIRES spi_flash esp_psram lvgl_init guider_ui)
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${APP_REQUIRES}
)
