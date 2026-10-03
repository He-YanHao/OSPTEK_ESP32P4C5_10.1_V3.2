#ifndef __TOUCH_JD9366_H__
#define __TOUCH_JD9366_H__


#include "esp_err.h"
#include "esp_lcd_touch.h"

// 初始化 I2C 总线 + touch 驱动
esp_err_t touch_jd9366_init(void);
// 
esp_lcd_touch_handle_t touch_jd9366_get_handle(void);

#endif