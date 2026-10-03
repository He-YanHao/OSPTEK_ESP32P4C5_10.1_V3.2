#include "lcd_jd9366.h"
#include "esp_lcd_jd9366.h"           // 控制器驱动
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_jd9366";

/* ---------------- 板级配置 ---------------- */
#define LCD_MIPI_DSI_LANE_NUM       2
#define LCD_MIPI_DSI_LANE_MBPS      1000

#define LCD_RST_GPIO                GPIO_NUM_27
#define LCD_BL_GPIO                 GPIO_NUM_26

#if LCD_COLOR_FORMAT == LCD_COLOR_FORMAT_RGB565
    #define LCD_BITS_PER_PIXEL      16
#else
    #define LCD_BITS_PER_PIXEL      24
#endif

/* ESP32-P4 内部 LDO 通道 3 给 MIPI D-PHY 供电 */
#define LCD_LDO_CHAN_ID             3
#define LCD_LDO_VOLTAGE_MV          2500

/* 背光 PWM */
#define LCD_BL_LEDC_TIMER           LEDC_TIMER_0
#define LCD_BL_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL         LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES        LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_FREQ_HZ         20000

/* ---------------- 内部句柄 ---------------- */
static esp_ldo_channel_handle_t  s_ldo_phy  = NULL;
static esp_lcd_dsi_bus_handle_t  s_dsi_bus  = NULL;
static esp_lcd_panel_io_handle_t s_io       = NULL;
static esp_lcd_panel_handle_t    s_panel    = NULL;
static bool                      s_bl_inited = false;

/* ---------------- 背光 ---------------- */
static esp_err_t lcd_backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LCD_BL_LEDC_MODE,
        .timer_num       = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz         = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc timer cfg failed");

    ledc_channel_config_t ccfg = {
        .gpio_num   = LCD_BL_GPIO,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel    = LCD_BL_LEDC_CHANNEL,
        .timer_sel  = LCD_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc channel cfg failed");

    s_bl_inited = true;
    return ESP_OK;
}

esp_err_t lcd_jd9366_backlight_set(uint8_t percent)
{
    if (!s_bl_inited) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;

    const uint32_t max_duty = (1u << LCD_BL_LEDC_DUTY_RES) - 1;
    uint32_t duty = (uint32_t)percent * max_duty / 100u;

    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty),
                        TAG, "ledc set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL),
                        TAG, "ledc update duty failed");
    return ESP_OK;
}

/* ---------------- 主初始化 ---------------- */
esp_err_t lcd_jd9366_init(uint8_t num_fbs)
{
    /* 1) MIPI D-PHY 供电 */
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = LCD_LDO_CHAN_ID,
        .voltage_mv = LCD_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_phy),
                        TAG, "acquire LDO for MIPI PHY failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2) DSI 总线 */
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = LCD_MIPI_DSI_LANE_NUM,
        .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus),
                        TAG, "new DSI bus failed");

    /* 3) DBI IO（命令通道） */
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_io),
                        TAG, "new DBI IO failed");

    /* 4) DPI 视频通道配置，num_fbs 由调用方传入 */
    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9366_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_DPI_IN_FORMAT, num_fbs);

    /* 5) vendor config */
    jd9366_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = s_dsi_bus,
            .dpi_config = &dpi_cfg,
        },
        .init_cmds      = NULL,
        .init_cmds_size = 0,
    };

    /* 6) panel dev config */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor_cfg,
    };

    /* 7) 创建 JD9366 面板 */
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9366(s_io, &panel_cfg, &s_panel),
                        TAG, "new JD9366 panel failed");

    /* 8) reset + init */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(s_panel),
                        TAG, "enable dma2d failed");

    /* 9) 背光 */
    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(lcd_jd9366_backlight_set(100), TAG, "backlight set failed");

    /* 10) 开显示 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "disp on failed");

    ESP_LOGI(TAG, "lcd jd9366 init ok (%dx%d), fbs=%d",
             LCD_WIDTH, LCD_HEIGHT, num_fbs);
    return ESP_OK;
}

/* ---------------- 句柄 ---------------- */
esp_lcd_panel_handle_t lcd_jd9366_get_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t lcd_jd9366_get_io(void) { return s_io; }

/* ---------------- 取帧缓冲 ---------------- */
esp_err_t lcd_jd9366_get_frame_buffers(void **fb0, void **fb1)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    if (fb0) *fb0 = NULL;
    if (fb1) *fb1 = NULL;

    /* 若 num_fbs=2，分别取 index 0/1 */
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, fb0);
    if (err != ESP_OK || !fb0) return err;

    if (fb1) {
        esp_err_t err2 = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, fb0, fb1);
        if (err2 != ESP_OK) {
            ESP_LOGW(TAG, "only one fb available");
            *fb1 = NULL;
        }
    }
    return ESP_OK;
}