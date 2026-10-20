#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "lcd_jd9366.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 I2C 总线 + touch 驱动
esp_err_t touch_jd9366_init(void);
// 获取触摸句柄
esp_lcd_touch_handle_t touch_jd9366_get_handle(void);

#ifdef __cplusplus
}
#endif