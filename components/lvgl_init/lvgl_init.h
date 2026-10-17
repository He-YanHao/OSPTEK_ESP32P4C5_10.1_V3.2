#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define H_RES       800
#define V_RES       1280

#ifdef __cplusplus
extern "C" {
#endif

// 配置LVGL初始化
esp_err_t lvgl_init(void);
// 锁定LVGL的刷新
bool lvgl_lock(uint32_t timeout_ms);
// 解锁LVGL
void lvgl_unlock(void);

#ifdef __cplusplus
}
#endif