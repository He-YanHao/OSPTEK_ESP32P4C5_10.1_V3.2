#include "sdkconfig.h"
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_APP_MODE_CAMERA_RAW)
#include "lcd_jd9366.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#else
#include "lvgl_init.h"
#include "lvgl.h"
#include "gui_guider.h"
#include "gg_utils.h"
#endif

static const char *TAG = "main";

#if !defined(CONFIG_APP_MODE_CAMERA_RAW)
static gg_ui_t guider_ui;
#endif

#if defined(CONFIG_APP_MODE_CAMERA_RAW)

/*
 * S0: pure bare-metal display check, no LVGL in the whole dependency chain.
 *
 * Two DPI-internal framebuffers are filled with the same RGB565 color bar,
 * and the band phase scrolls every 2 seconds. If the picture moves, the
 * "CPU writes framebuffer -> DSI scanout" path works. This is exactly the
 * memory that the camera frames will be written to in stage S2.
 */
#define RAW_FB_NUM 2

static const uint16_t s_color_bar[8] = {
    0xFFFF, /* white   */
    0xFFE0, /* yellow  */
    0x07FF, /* cyan    */
    0x07E0, /* green   */
    0xF81F, /* magenta */
    0xF800, /* red     */
    0x001F, /* blue    */
    0x0000, /* black   */
};

static void raw_fill_color_bar(uint16_t *fb, int w, int h, int phase)
{
    uint16_t line[LCD_WIDTH];
    for (int x = 0; x < w; x++) {
        line[x] = s_color_bar[((x * 8 / w) + phase) & 0x7];
    }
    for (int y = 0; y < h; y++) {
        memcpy(fb + (size_t)y * w, line, (size_t)w * sizeof(uint16_t));
    }
}

static void app_main_raw(void)
{
    ESP_ERROR_CHECK(lcd_jd9366_init(RAW_FB_NUM));

    esp_lcd_panel_handle_t panel = lcd_jd9366_get_panel();
    ESP_LOGI(TAG, "S0: panel handle = %p", panel);

    void *fb[RAW_FB_NUM] = {NULL, NULL};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, RAW_FB_NUM,
                                                       &fb[0], &fb[1]));
    for (int i = 0; i < RAW_FB_NUM; i++) {
        ESP_LOGI(TAG, "S0: fb[%d] = %p", i, fb[i]);
    }

    /* Phase 1: DSI controller built-in pattern, zero CPU involvement.
     * Confirms D-PHY / DSI / panel chain independently of any framebuffer. */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    ESP_LOGI(TAG, "S0: DSI built-in test pattern ON (3s)");
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_NONE));
    ESP_LOGI(TAG, "S0: test pattern OFF, switching to framebuffer content");

    /* Phase 2: write RGB565 into the DPI framebuffers and keep them scrolling. */
    const size_t fb_size = (size_t)LCD_WIDTH * LCD_HEIGHT * LCD_BYTES_PER_PIXEL;
    int phase = 0;
    while (1) {
        for (int i = 0; i < RAW_FB_NUM; i++) {
            raw_fill_color_bar((uint16_t *)fb[i], LCD_WIDTH, LCD_HEIGHT, phase);
            ESP_ERROR_CHECK(esp_cache_msync(fb[i], fb_size,
                                            ESP_CACHE_MSYNC_FLAG_DIR_C2M));
        }
        ESP_LOGI(TAG, "S0: color bar phase=%d, fb_size=%u bytes",
                 phase, (unsigned)fb_size);
        phase = (phase + 1) & 0x7;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

#endif /* CONFIG_APP_MODE_CAMERA_RAW */

void app_main(void)
{
#if defined(CONFIG_APP_MODE_CAMERA_RAW)
    app_main_raw();
#else
    lvgl_init();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        setup_ui(&guider_ui);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "UI launched");
#endif
}