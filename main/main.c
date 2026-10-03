#include "lvgl_init.h"
#include "lvgl.h"
#include "touch_jd9366.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gui_guider.h"
#include "gg_utils.h"


static const char *TAG = "main";

gg_ui_t guider_ui;

void app_main(void)
{
    ESP_ERROR_CHECK(lvgl_init());

    if (lvgl_lock(0)) {
        setup_ui(&guider_ui);
        lvgl_unlock();
    }

    ESP_LOGI(TAG, "UI launched");
}