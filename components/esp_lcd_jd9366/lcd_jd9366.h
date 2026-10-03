#ifndef __LCD_JD9366_H__
#define __LCD_JD9366_H__

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

// 颜色格式
#define LCD_COLOR_FORMAT_RGB565   1
#define LCD_COLOR_FORMAT_RGB888   2

// 修改像素格式
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

// 分辨率
#define LCD_WIDTH                   800
#define LCD_HEIGHT                  1280


// 初始化
esp_err_t lcd_jd9366_init(void);

/* 背光亮度，0~100 */
esp_err_t lcd_jd9366_backlight_set(uint8_t percent);

/* 面板句柄（给 LVGL 用） */
esp_lcd_panel_handle_t lcd_jd9366_get_panel(void);

/* DBI 命令通道句柄（发 LCD 命令用） */
esp_lcd_panel_io_handle_t lcd_jd9366_get_io(void);

/* 取 DPI 面板的帧缓冲（num_fbs=2 时可用；返回 0/1 两块）*/
esp_err_t lcd_jd9366_get_frame_buffers(void **fb0, void **fb1);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_JD9366_H__ */