#include "touch_jd9366.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_jd9366.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_jd9366";

/* ========== 板级配置 ========== */
#define TP_I2C_PORT         I2C_NUM_0
#define TP_I2C_SCL_GPIO     GPIO_NUM_8
#define TP_I2C_SDA_GPIO     GPIO_NUM_7
#define TP_I2C_FREQ_HZ      400000

#define TP_RST_GPIO         GPIO_NUM_NC     // 若共用 LCD RST 就填同一个脚
#define TP_INT_GPIO         GPIO_NUM_NC

#define TP_X_MAX            800
#define TP_Y_MAX            1280

/* ========== 句柄 ========== */
static i2c_master_bus_handle_t   s_i2c_bus   = NULL;
static esp_lcd_panel_io_handle_t s_tp_io     = NULL;
static esp_lcd_touch_handle_t    s_tp        = NULL;

esp_err_t touch_jd9366_init(void)
{
    esp_err_t ret = ESP_OK;

    /* 1) I2C 主机总线 */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = TP_I2C_PORT,
        .scl_io_num                   = TP_I2C_SCL_GPIO,
        .sda_io_num                   = TP_I2C_SDA_GPIO,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus),
                      err, TAG, "i2c new master bus failed");

    /* 2) 触摸 IO */
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_JD9366_CONFIG();
    tp_io_cfg.scl_speed_hz = TP_I2C_FREQ_HZ;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &s_tp_io),
                      err, TAG, "new touch io failed");

    /* 3) 触摸配置 */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = TP_X_MAX,
        .y_max        = TP_Y_MAX,
        .rst_gpio_num = TP_RST_GPIO,
        .int_gpio_num = TP_INT_GPIO,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* 4) 创建驱动 */
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_jd9366(s_tp_io, &tp_cfg, &s_tp),
                      err, TAG, "new jd9366 touch failed");

    ESP_LOGI(TAG, "touch jd9366 init ok");
    return ESP_OK;

err:
    if (s_tp_io)  { esp_lcd_panel_io_del(s_tp_io); s_tp_io = NULL; }
    if (s_i2c_bus) { i2c_del_master_bus(s_i2c_bus); s_i2c_bus = NULL; }
    return ret;
}

esp_lcd_touch_handle_t touch_jd9366_get_handle(void)
{
    return s_tp;
}