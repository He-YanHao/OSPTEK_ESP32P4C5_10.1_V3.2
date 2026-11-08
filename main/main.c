#include "sdkconfig.h"
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_APP_MODE_CAMERA_RAW)
#include "lcd_jd9366.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "camera_preview.h"
#else
#include "lvgl_init.h"
#include "lvgl.h"
#include "gui_guider.h"
#include "gg_utils.h"
#endif

static const char *TAG = "main";

#if !defined(CONFIG_APP_MODE_CAMERA_RAW)
static gg_ui_t guider_ui;
#endif

#if defined(CONFIG_APP_MODE_CAMERA_RAW)

/*
 * 裸显模式：整条 LVGL / adapter / guider 链不参与构建。
 *
 * 流程：面板初始化 -> DSI 内置图案自检 3 秒 -> 摄像头直显（CSI + ISP -> DSI 帧缓冲）。
 * 画面通路在 camera_preview 组件里，本文件只负责启动与心跳。
 */
#define RAW_FB_NUM 2

static void app_main_raw(void)
{
    ESP_ERROR_CHECK(lcd_jd9366_init(RAW_FB_NUM));

    esp_lcd_panel_handle_t panel = lcd_jd9366_get_panel();
    ESP_LOGI(TAG, "panel handle = %p", panel);

    /* 开机自检：DSI 内置图案不经过帧缓冲，可独立确认 D-PHY / DSI / 屏 通路。
     * 图案消失后画面交给摄像头。 */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    ESP_LOGI(TAG, "boot self-test: DSI vertical bars for 3s");
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_NONE));

    /* cfg=NULL 走本板默认：SC2336 800x800@30fps RAW8，SCCB GPIO7/8，垂直居中显示 */
    esp_err_t err = camera_preview_start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_preview_start failed: %s", esp_err_to_name(err));
        return;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "camera frames: %u", (unsigned)camera_preview_get_frame_count());
    }
}

#endif /* CONFIG_APP_MODE_CAMERA_RAW */

void app_main(void)
{
#if defined(CONFIG_APP_MODE_CAMERA_RAW)
    app_main_raw();
#else
    lvgl_init();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        setup_ui(&guider_ui);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "UI launched");
#endif
}
