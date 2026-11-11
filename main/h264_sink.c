/*
 * h264_sink —— 把 camera_preview 的 RGB565 帧喂给 P4 硬件 H.264 编码器，
 *              再把 Annex-B NAL 交给 RTSP Server。
 *
 * 为什么用 RGB565_LE：esp_h264 1.5.0 的 HW encoder 明确支持该输入格式，
 * 因此无需改 CSI/ISP 配置、无需 esp_video V4L2 重写，也不丢弃已跑通的
 * 屏幕预览链。DPI 帧缓冲中部就是一块连续 800x800 RGB565，编码零拷贝。
 *
 * 码流验收走 RTSP 拉流（ffprobe / ffmpeg），S3 曾用过的 TCP:9999 dump 与
 * 串口 NALHEX 分块导出已在 S4 收尾时移除：前者常驻任务占栈和 512 KB
 * PSRAM，后者会在首帧阻塞帧提交任务约 0.8 s。
 */

#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"

#include "camera_preview.h"
#include "h264_sink.h"
#include "rtsp_server.h"

#define ENC_W          800
#define ENC_H          800
#define ENC_FPS        30
#define ENC_GOP        30
#define ENC_IN_BYTES   (ENC_W * ENC_H * 2)

static const char *TAG = "h264_sink";

static esp_h264_enc_handle_t s_enc;
static uint8_t *s_out_buf;
static uint32_t s_out_cap;

static volatile uint32_t s_frames;
static volatile uint32_t s_bytes;
static volatile uint32_t s_idr;
static volatile uint32_t s_errs;
static bool s_idr_logged;

static void h264_on_frame(const uint8_t *frame, size_t len, void *user_ctx)
{
    (void)user_ctx;
    if (!s_enc || !frame || len < ENC_IN_BYTES) {
        s_errs++;
        return;
    }

    esp_h264_enc_in_frame_t in = {
        .raw_data = { .buffer = (uint8_t *)frame, .len = ENC_IN_BYTES },
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
    };
    esp_h264_enc_out_frame_t out = {
        .raw_data = { .buffer = s_out_buf, .len = s_out_cap },
    };

    esp_h264_err_t ret = esp_h264_enc_process(s_enc, &in, &out);
    if (ret != ESP_H264_ERR_OK) {
        s_errs++;
        if (s_errs <= 3) {
            ESP_LOGE(TAG, "enc_process failed ret=%d", (int)ret);
        }
        return;
    }
    if (out.length == 0 || out.length > s_out_cap) {
        s_errs++;
        return;
    }

    s_frames++;
    s_bytes += out.length;

    /* 把 Annex-B NAL 交给 RTSP Server（非阻塞，队列满即丢帧）。 */
    rtsp_server_push_frame(s_out_buf, out.length,
                           out.frame_type == ESP_H264_FRAME_TYPE_IDR);

    if (out.frame_type == ESP_H264_FRAME_TYPE_IDR) {
        s_idr++;
        if (!s_idr_logged) {
            /* 只打一行、只打一次：确认首帧带 SPS/PPS，不阻塞帧提交。 */
            char head[97];
            const uint32_t n = out.length < 32 ? out.length : 32;
            for (uint32_t i = 0; i < n; i++) {
                snprintf(head + i * 3, sizeof(head) - i * 3, "%02x ",
                         s_out_buf[i]);
            }
            ESP_LOGI(TAG, "first IDR NAL len=%u head=%s",
                     (unsigned)out.length, head);
            s_idr_logged = true;
        }
    }
}

esp_err_t h264_sink_start(void)
{
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_RGB565_LE,
        .gop = ENC_GOP,
        .fps = ENC_FPS,
        .res = { .width = ENC_W, .height = ENC_H },
        .rc = { .bitrate = 2 * 1000 * 1000, .qp_min = 24, .qp_max = 36 },
    };

    esp_h264_err_t ret = esp_h264_enc_hw_new(&cfg, &s_enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new failed ret=%d", (int)ret);
        return ESP_FAIL;
    }
    ret = esp_h264_enc_open(s_enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed ret=%d", (int)ret);
        return ESP_FAIL;
    }

    uint32_t actual = 0;
    s_out_buf = esp_h264_aligned_calloc(16, 1, ENC_IN_BYTES, &actual,
                                        ESP_H264_MEM_SPIRAM);
    if (!s_out_buf) {
        ESP_LOGE(TAG, "encoder output buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_out_cap = actual;

    camera_preview_set_frame_cb(h264_on_frame, NULL);
    ESP_LOGI(TAG, "hw encoder ready: %dx%d RGB565_LE, out cap=%u, GOP=%d, fps=%d",
             ENC_W, ENC_H, (unsigned)s_out_cap, ENC_GOP, ENC_FPS);
    return ESP_OK;
}

void h264_sink_get_stats(uint32_t *frames, uint32_t *bytes,
                         uint32_t *idr, uint32_t *errors)
{
    if (frames) *frames = s_frames;
    if (bytes)  *bytes  = s_bytes;
    if (idr)    *idr    = s_idr;
    if (errors) *errors = s_errs;
}
