#include "lvgl_init.h"
#include "lvgl.h"
#include "gui_guider.h"
#include "gg_utils.h"
#include "esp_log.h"


static const char *TAG = "main";

gg_ui_t guider_ui;

void app_main(void)
{
    lvgl_init();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        setup_ui(&guider_ui);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "UI launched");
}