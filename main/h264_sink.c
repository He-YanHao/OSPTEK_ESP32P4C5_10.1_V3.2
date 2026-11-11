/*
 * h264_sink —— S3：把 camera_preview 的 RGB565 帧喂给 P4 硬件 H.264 编码器。
 *
 * 为什么用 RGB565_LE：esp_h264 1.5.0 的 HW encoder 明确支持该输入格式，
 * 因此无需改 CSI/ISP 配置、无需 esp_video V4L2 重写，也不丢弃已跑通的
 * 屏幕预览链。DPI 帧缓冲中部就是一块连续 800x800 RGB565，编码零拷贝。
 *
 * 验收：首个 IDR 帧（自带 SPS/PPS）通过 TCP:9999 完整发给 PC，
 *       PC 保存为 .h264 后用 ffprobe 解析，即为合法 Annex-B 码流。
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

#include "lwip/sockets.h"

#include "camera_preview.h"
#include "h264_sink.h"

#define ENC_W          800
#define ENC_H          800
#define ENC_FPS        30
#define ENC_GOP        30
#define ENC_IN_BYTES   (ENC_W * ENC_H * 2)
#define DUMP_PORT      9999
#define DUMP_BUF_CAP   (512 * 1024)

static const char *TAG = "h264_sink";

static esp_h264_enc_handle_t s_enc;
static uint8_t *s_out_buf;
static uint32_t s_out_cap;
static uint8_t *s_dump_buf;
static volatile uint32_t s_dump_len;
static volatile bool s_dump_ready;

static volatile uint32_t s_frames;
static volatile uint32_t s_bytes;
static volatile uint32_t s_idr;
static volatile uint32_t s_errs;

static void h264_dump_server(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(DUMP_PORT),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "NAL dump server listening on TCP:%d", DUMP_PORT);

    int fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0) {
        /* 等首个 IDR 帧编码完成 */
        TickType_t t0 = xTaskGetTickCount();
        while (!s_dump_ready && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(20000)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_dump_ready) {
            uint32_t sent = 0;
            while (sent < s_dump_len) {
                int n = send(fd, (const char *)s_dump_buf + sent,
                             s_dump_len - sent, 0);
                if (n <= 0) {
                    break;
                }
                sent += (uint32_t)n;
            }
            ESP_LOGI(TAG, "IDR NAL sent %u/%u bytes", (unsigned)sent,
                     (unsigned)s_dump_len);
        } else {
            ESP_LOGW(TAG, "no IDR frame within 20 s");
        }
        close(fd);
    }
    close(listen_fd);
    vTaskDelete(NULL);
}

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

    if (out.frame_type == ESP_H264_FRAME_TYPE_IDR) {
        s_idr++;
        if (!s_dump_ready && out.length <= DUMP_BUF_CAP) {
            char head[97];
            const uint32_t n = out.length < 32 ? out.length : 32;
            for (uint32_t i = 0; i < n; i++) {
                snprintf(head + i * 3, sizeof(head) - i * 3, "%02x ",
                         s_out_buf[i]);
            }
            ESP_LOGI(TAG, "first IDR NAL len=%u type=%d head=%s",
                     (unsigned)out.length, (int)out.frame_type, head);

            /* 串口分块导出完整 IDR NAL（32 字节/行 hex）。这条路径不依赖 Wi-Fi，
             * PC 侧把 NALHEX 行还原成 .h264 即可用 ffprobe 验证 Annex-B 合法性。 */
            for (uint32_t off = 0; off < out.length; off += 32) {
                uint32_t n = (out.length - off) < 32 ? (out.length - off) : 32;
                char line[65];
                for (uint32_t i = 0; i < n; i++) {
                    snprintf(line + i * 2, 3, "%02x", s_out_buf[off + i]);
                }
                line[n * 2] = '\0';
                ESP_LOGI(TAG, "NALHEX %04u %s", (unsigned)off, line);
            }

            memcpy(s_dump_buf, s_out_buf, out.length);
            s_dump_len = out.length;
            s_dump_ready = true;
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

    s_dump_buf = esp_h264_aligned_calloc(16, 1, DUMP_BUF_CAP, &actual,
                                         ESP_H264_MEM_SPIRAM);
    if (!s_dump_buf) {
        ESP_LOGE(TAG, "NAL dump buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    camera_preview_set_frame_cb(h264_on_frame, NULL);
    ESP_LOGI(TAG, "hw encoder ready: %dx%d RGB565_LE, out cap=%u, GOP=%d, fps=%d",
             ENC_W, ENC_H, (unsigned)s_out_cap, ENC_GOP, ENC_FPS);

    if (xTaskCreate(h264_dump_server, "h264_dump", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dump server task create failed");
        return ESP_FAIL;
    }
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
