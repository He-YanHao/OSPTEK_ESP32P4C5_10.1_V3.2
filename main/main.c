#include "lvgl_init.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static int s_count = 0;
static lv_obj_t *s_count_label = NULL;

// 按钮回调
static void btn_event_cb(lv_event_t *e)
{
    (void)e;
    s_count++;
    if (s_count_label) {
        lv_label_set_text_fmt(s_count_label, "Count: %d", s_count);
    }
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "JD9366 + LVGL");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    // 按钮
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 200, 80);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2196F3), 0);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click Me");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_label);

    // 计数器
    s_count_label = lv_label_create(scr);
    lv_label_set_text(s_count_label, "Count: 0");
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_count_label, LV_ALIGN_BOTTOM_MID, 0, -40);

    // 注册回调
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(lvgl_init());

    if (lvgl_lock(-1)) {
        create_ui();
        lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "failed to lock lvgl");
        return;
    }

    ESP_LOGI(TAG, "app_main done");
}