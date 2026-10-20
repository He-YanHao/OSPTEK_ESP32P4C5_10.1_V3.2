#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "lvgl.h"
#include "esp_lv_adapter.h"

// 配置LVGL初始化
esp_err_t lvgl_init(void);

#ifdef __cplusplus
}
#endif