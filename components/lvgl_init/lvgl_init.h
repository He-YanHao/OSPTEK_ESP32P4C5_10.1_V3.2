#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define H_RES       800
#define V_RES       1280

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lvgl_init(void);
lv_display_t *lvgl_get_disp(void);
bool lvgl_lock(uint32_t timeout_ms);
void lvgl_unlock(void);

#ifdef __cplusplus
}
#endif