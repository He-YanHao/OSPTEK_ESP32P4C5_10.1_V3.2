# camera_preview：SC2336 -> CSI -> ISP -> DSI 帧缓冲 的本地直显
#
# 不依赖 LVGL / esp_lvgl_adapter / guider。
# MINIMAL_BUILD ON 下所有依赖必须显式列出（含托管组件的命名空间名）。
idf_component_register(
    SRCS "camera_preview.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lcd_jd9366
        esp_lcd
        esp_mm
        esp_driver_isp
        esp_driver_cam
        esp_driver_i2c
        espressif__esp_cam_sensor
        espressif__esp_sccb_intf
)
