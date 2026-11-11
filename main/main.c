#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_APP_MODE_NET) || defined(CONFIG_APP_MODE_H264)
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "lwip/ip_addr.h"
#endif

#if defined(CONFIG_APP_MODE_CAMERA_RAW) || defined(CONFIG_APP_MODE_H264)
#include "camera_preview.h"
#endif

#if defined(CONFIG_APP_MODE_CAMERA_RAW) || defined(CONFIG_APP_MODE_H264)
#include "lcd_jd9366.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#endif

#if defined(CONFIG_APP_MODE_H264)
#include "h264_sink.h"
#include "rtsp_server.h"
#endif

#if !defined(CONFIG_APP_MODE_CAMERA_RAW) && !defined(CONFIG_APP_MODE_NET) && !defined(CONFIG_APP_MODE_H264)
#include "lvgl_init.h"
#include "lvgl.h"
#include "gui_guider.h"
#include "gg_utils.h"
#endif

static const char *TAG = "main";

#if !defined(CONFIG_APP_MODE_CAMERA_RAW) && !defined(CONFIG_APP_MODE_NET) && !defined(CONFIG_APP_MODE_H264)
gg_ui_t guider_ui;
#endif

#if defined(CONFIG_APP_MODE_CAMERA_RAW)

/*
 * 裸显模式：整条 LVGL / adapter / guider 链不参与构建。
 *
 * 流程：面板初始化 -> 摄像头直显（CSI + ISP -> DSI 帧缓冲）。
 * 画面通路在 camera_preview 组件里，本文件只负责启动与心跳。
 */
#define RAW_FB_NUM 2

static void app_main_raw(void)
{
    ESP_ERROR_CHECK(lcd_jd9366_init(RAW_FB_NUM));

    esp_lcd_panel_handle_t panel = lcd_jd9366_get_panel();
    ESP_LOGI(TAG, "panel handle = %p", panel);

    /* 不做 DSI 内置彩条自检：pattern -> framebuffer 的模式切换在部分上电状态下
     * 结果不稳定，会残留彩条 / 条纹错位直至黑屏。D-PHY / DSI / 面板通路已由
     * GUI 基线反复验证，这里让 DPI 从初始化起就始终保持在 framebuffer 模式。 */
    ESP_LOGI(TAG, "skip DSI pattern self-test (framebuffer mode only)");

    /* cfg=NULL 走本板默认：SC2336 800x800@30fps RAW8，SCCB GPIO7/8，垂直居中显示 */
    esp_err_t err = camera_preview_start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_preview_start failed: %s", esp_err_to_name(err));
        return;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "camera frames: %u", (unsigned)camera_preview_get_frame_count());
    }
}

#endif /* CONFIG_APP_MODE_CAMERA_RAW */

#if defined(CONFIG_APP_MODE_NET) || defined(CONFIG_APP_MODE_H264)

/*
 * S2 网络基线：ESP-Hosted 在 app_main 之前自动完成 SDIO 握手 / RPC 协商 /
 * Wi-Fi remote 初始化（CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN）。
 * 这里只跑标准 esp_netif + Wi-Fi STA；拿到 IP 即证明 C5 网卡化在本工程内可用。
 */
static void app_net_wifi_event_handler(void *arg, esp_event_base_t base,
                                       int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "wifi sta start, connecting");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR ", mask: " IPSTR ", gw: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
                 IP2STR(&ev->ip_info.gw));
    }
}

static void app_net_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_t got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &app_net_wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &app_net_wifi_event_handler, NULL, &got_ip));

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", CONFIG_APP_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", CONFIG_APP_WIFI_PASSWORD);
    /* 照抄官方 sta/mcu_host 示例 S1 实测成功的配置：WPA2_PSK threshold
     * + SAE PWE BOTH + H2E identifier。热点 OnePlus 为纯 WPA3-SAE
     * (RSN AKM 00-0f-ac:8)，threshold 只作扫描下限，SAE 协商靠 PWE。 */
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    snprintf((char *)wc.sta.sae_h2e_identifier,
             sizeof(wc.sta.sae_h2e_identifier), "%s", "");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 绝不在这里打印 SSID / 密码（官方示例会明文打印，已刻意去掉） */
    ESP_LOGI(TAG, "wifi station started (credentials not logged)");
}

#endif /* NET || H264 */

#if defined(CONFIG_APP_MODE_H264)

/* S3：网络 + 摄像头 + P4 硬件 H.264 编码。
 * 顺序不能反：h264_sink_start() 要先注册帧回调，再启动采集。 */
static void app_main_h264(void)
{
    app_net_init();

    /* camera_preview 由 DMA 直写 DPI 帧缓冲，必须先初始化面板。 */
    ESP_ERROR_CHECK(lcd_jd9366_init(2));

    if (h264_sink_start() != ESP_OK) {
        ESP_LOGE(TAG, "h264_sink_start failed");
        return;
    }

    /* S4：RTSP Server 必须在采集启动前就绪，避免首帧 NAL 丢失。 */
    if (rtsp_server_start(8554) != ESP_OK) {
        ESP_LOGE(TAG, "rtsp_server_start failed");
        return;
    }

    esp_err_t err = camera_preview_start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_preview_start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "H264 + RTSP started (rtsp://<ip>:8554/live)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t frames = 0, bytes = 0, idr = 0, errors = 0;
        h264_sink_get_stats(&frames, &bytes, &idr, &errors);
        uint32_t sess = 0, pkts = 0, rbytes = 0, dropped = 0;
        rtsp_server_get_stats(&sess, &pkts, &rbytes, &dropped);
        ESP_LOGI(TAG, "h264=%u idr=%u err=%u cam=%u | rtsp sess=%u pkts=%u bytes=%u drop=%u",
                 (unsigned)frames, (unsigned)idr, (unsigned)errors,
                 (unsigned)camera_preview_get_frame_count(),
                 (unsigned)sess, (unsigned)pkts, (unsigned)rbytes,
                 (unsigned)dropped);
    }
}

#endif /* CONFIG_APP_MODE_H264 */

void app_main(void)
{
#if defined(CONFIG_APP_MODE_CAMERA_RAW)
    app_main_raw();
#elif defined(CONFIG_APP_MODE_H264)
    app_main_h264();
#elif defined(CONFIG_APP_MODE_NET)
    app_net_init();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "net heartbeat");
    }
#else
    lvgl_init();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        setup_ui(&guider_ui);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "UI launched");
#endif
}
