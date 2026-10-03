#include "lvgl_init.h"
#include "lvgl.h"
#include "touch_jd9366.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 触摸点标记和坐标标签
static lv_obj_t *s_dot   = NULL;
static lv_obj_t *s_label = NULL;

// 全屏点击回调：把红点移动到点击位置
static void scr_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;

        lv_point_t p;
        lv_indev_get_point(indev, &p);

        // 把红点移到点击位置（点中心对齐）
        lv_obj_set_pos(s_dot, p.x - 15, p.y - 15);
        lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

        // 更新坐标显示
lv_label_set_text_fmt(s_label, "x=%d y=%d", (int)p.x, (int)p.y);
    } else if (code == LV_EVENT_RELEASED) {
        // 松手后隐藏红点，但保留坐标标签
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // 整屏可点击
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, scr_event_cb, LV_EVENT_ALL, NULL);

    // 坐标标签，显示在顶部
    s_label = lv_label_create(scr);
    lv_label_set_text(s_label, "x=- y=-");
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, 10);

    // 四角标记，用于对照方向
    const struct { int x, y; uint32_t color; const char *name; } corners[] = {
        {0,    0,    0xFF0000, "TL"},
        {1265, 0,    0x00FF00, "TR"},
        {0,    785,  0x0000FF, "BL"},
        {1265, 785,  0xFFFF00, "BR"},
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = lv_obj_create(scr);
        lv_obj_set_size(c, 15, 15);
        lv_obj_set_pos(c, corners[i].x, corners[i].y);
        lv_obj_set_style_bg_color(c, lv_color_hex(corners[i].color), 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *t = lv_label_create(scr);
        lv_label_set_text(t, corners[i].name);
        lv_obj_set_style_text_color(t, lv_color_hex(corners[i].color), 0);
        lv_obj_set_pos(t, corners[i].x + 20, corners[i].y + 20);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    }

    // 触摸点红点，初始隐藏
    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, 30, 30);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_width(s_dot, 2, 0);
    lv_obj_set_style_border_color(s_dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

void app_main(void)
{
    ESP_ERROR_CHECK(lvgl_init());

    if (lvgl_lock(0)) {
        create_ui();
        lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "failed to lock lvgl");
        return;
    }

    ESP_LOGI(TAG, "app_main done");
}