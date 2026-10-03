#include "lvgl_init.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "lcd_jd9366.h"
#include "touch_jd9366.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl_init";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_display_t          *s_disp  = NULL;

#define ROTATION_ANGLE    ESP_LV_ADAPTER_ROTATE_90
#define TEAR_AVOID_MODE   ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI

esp_err_t lvgl_init(void)
{
    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
                          TEAR_AVOID_MODE, ROTATION_ANGLE);
    ESP_LOGI(TAG, "required frame buffers: %d", num_fbs);

    ESP_RETURN_ON_ERROR(lcd_jd9366_init(num_fbs), TAG, "lcd init failed");
    ESP_RETURN_ON_ERROR(touch_jd9366_init(),      TAG, "touch init failed");

    s_panel = lcd_jd9366_get_panel();
    s_touch = touch_jd9366_get_handle();
    ESP_RETURN_ON_FALSE(s_panel && s_touch, ESP_ERR_INVALID_STATE, TAG, "handles null");

    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "adapter init failed");

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
            s_panel, NULL, H_RES, V_RES, ROTATION_ANGLE);
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "register display failed");

    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, s_touch);
    lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_NO_MEM, TAG, "register touch failed");

    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "adapter start failed");

    ESP_LOGI(TAG, "LVGL adapter init ok, rotation=%d, fbs=%d", ROTATION_ANGLE, num_fbs);
    return ESP_OK;
}

lv_display_t *lvgl_get_disp(void) { return s_disp; }

bool lvgl_lock(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return esp_lv_adapter_lock(ticks) == ESP_OK;
}

void lvgl_unlock(void)
{
    esp_lv_adapter_unlock();
}