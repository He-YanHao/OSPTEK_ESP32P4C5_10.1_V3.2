#ifndef __LCD_JD9366_H__
#define __LCD_JD9366_H__

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

// 定义颜色格式的宏
#define LCD_COLOR_FORMAT_RGB565   1
#define LCD_COLOR_FORMAT_RGB888   2

// 使用的像素格式
#define LCD_COLOR_FORMAT          LCD_COLOR_FORMAT_RGB565

#if LCD_COLOR_FORMAT == LCD_COLOR_FORMAT_RGB565
    #define LCD_BYTES_PER_PIXEL     2
    #define LCD_DPI_IN_FORMAT       LCD_COLOR_FMT_RGB565
#elif LCD_COLOR_FORMAT == LCD_COLOR_FORMAT_RGB888
    #define LCD_BYTES_PER_PIXEL     3
    #define LCD_DPI_IN_FORMAT       LCD_COLOR_FMT_RGB888
#else
    #error "Unsupported LCD_COLOR_FORMAT"
#endif

// 分辨率 固定为800x1280 只能使用PPA进行旋转 无法使用指令硬件转屏
#define LCD_WIDTH                   800
#define LCD_HEIGHT                  1280

// 初始化
esp_err_t lcd_jd9366_init(uint8_t num_fbs);

// 背光亮度 0~100
esp_err_t lcd_jd9366_backlight_set(uint8_t percent);

// 面板句柄
esp_lcd_panel_handle_t lcd_jd9366_get_panel(void);

// DBI 命令通道句柄（发 LCD 命令用）
esp_lcd_panel_io_handle_t lcd_jd9366_get_io(void);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_JD9366_H__ */