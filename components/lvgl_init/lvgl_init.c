#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_init.h"
#include "lvgl.h"
#include "lcd_jd9366.h"
#include "touch_jd9366.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "lvgl_init";

static esp_lcd_panel_handle_t    s_panel = NULL; // 配置 LCD 面板操作句柄
static esp_lcd_panel_io_handle_t s_io    = NULL; // 配置 MIPI IO 操作句柄
static esp_lcd_touch_handle_t    s_touch = NULL; // 配置触摸操作句柄
static lv_display_t              *s_disp = NULL; // 配置 LVGL 显示面板

// 配置 LVGL 适配层的旋转角度 顺时针旋转
#define ROTATION_ANGLE_DEG   270
// 配置显示模式 ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI 在当前版本默认配置为防撕裂模式
#define TEAR_AVOID_MODE   ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI
// 通过这个宏定义把旋转角度转化为对应的枚举
// 因为 esp_lvgl_adapter 里代表屏幕旋转角度的 esp_lv_adapter_rotation_t 是枚举类型 无法在宏定义时比较
// 因此 需要这样迂回处理
#if   ROTATION_ANGLE_DEG == 0
    #define ROTATION_ANGLE   ESP_LV_ADAPTER_ROTATE_0
#elif ROTATION_ANGLE_DEG == 90
    #define ROTATION_ANGLE   ESP_LV_ADAPTER_ROTATE_90
#elif ROTATION_ANGLE_DEG == 180
    #define ROTATION_ANGLE   ESP_LV_ADAPTER_ROTATE_180
#elif ROTATION_ANGLE_DEG == 270
    #define ROTATION_ANGLE   ESP_LV_ADAPTER_ROTATE_270
#else
    #error "Unsupported ROTATION_ANGLE_DEG"
#endif

// 根据当前的旋转角度和 LCD 本身的宽和高 配置 LVGL 缓冲区的宽和高
#if (ROTATION_ANGLE_DEG == 0) || (ROTATION_ANGLE_DEG == 180)
    #define H_RES  LCD_HEIGHT
    #define V_RES  LCD_WIDTH
#elif (ROTATION_ANGLE_DEG == 90) || (ROTATION_ANGLE_DEG == 270)
    #define H_RES  LCD_WIDTH
    #define V_RES  LCD_HEIGHT
#endif

/**
 * @brief 配置LVGL初始化
 * 
 * @return esp_err_t 
 */
esp_err_t lvgl_init(void)
{
    // 获取需要分配多少个帧缓冲 这个是纯粹的计算 不涉及任何的硬件配置 单纯获取一个缓冲区数量
    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
                          TEAR_AVOID_MODE, ROTATION_ANGLE);
    ESP_LOGI(TAG, "required frame buffers: %d", num_fbs);

    // 依次初始化显示和触摸
    ESP_RETURN_ON_ERROR(lcd_jd9366_init(num_fbs), TAG, "lcd init failed");
    ESP_RETURN_ON_ERROR(touch_jd9366_init(),      TAG, "touch init failed");

    // 获取操作句柄
    s_panel = lcd_jd9366_get_panel();
    s_io    = lcd_jd9366_get_io();
    s_touch = touch_jd9366_get_handle();
    ESP_RETURN_ON_FALSE(s_panel && s_touch, ESP_ERR_INVALID_STATE, TAG, "handles null");

    // 初始化 esp_lv_adapter 适配层
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(); // 获取默认配置
    cfg.task_core_id  = 1;  // 绑定在内核1
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "adapter init failed");

    // 将初始化好的 LCD 面板操作句柄 和 MIPI IO 操作句柄 交给 LVGL
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
            s_panel, s_io, H_RES, V_RES, ROTATION_ANGLE);
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "register display failed");

    // 将初始化好的触摸操作句柄交给 LVGL
    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, s_touch);
    lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_NO_MEM, TAG, "register touch failed");

    // 启动LVGL适配层
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "adapter start failed");

    ESP_LOGI(TAG, "LVGL adapter init ok, rotation=%d, fbs=%d", ROTATION_ANGLE, num_fbs);
    return ESP_OK;
}