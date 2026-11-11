/*
 * camera_preview —— SC2336 -> MIPI-CSI -> ISP -> DSI 帧缓冲，零拷贝直显
 *
 * 数据路径（零拷贝）：SC2336 800x800 RAW8@30fps -> MIPI-CSI(bypass)
 * -> ISP(RAW8->RGB565 + demosaic) -> DW_GDMA 直写 DPI 帧缓冲中部
 * -> esp_lcd_panel_draw_bitmap() 提交并切换 cur_fb_index -> DSI 显示。
 * ISP 侧补齐 WBG / gamma / color 色彩链，避免只开 demosaic 时偏绿发暗。
 *
 * 关键约束（详见 doc/摄像头裸显预览方案.md 第 8 节）：
 *   1) CSI 必须 RAW8 -> RAW8（src==dst 自动 bypass），RAW8->RGB565 交给 ISP；
 *      自定义 data_type.bits_per_pixel 会让 DMA 按错误 bpp 期望数据量。
 *   2) lane_bit_rate_mbps / data_lane_num 取自传感器格式自带的 mipi_info，
 *      不能照抄其它传感器示例的固定值（SC2336 800x800 为 336 Mbps / 2 lane）。
 *   3) 双帧缓冲必须每帧调 esp_lcd_panel_draw_bitmap() 提交，DPI 的
 *      cur_fb_index 只在提交时更新，初始化后恒为 0。
 *   4) frames 计数只是 DMA 定长传输完成，不代表有画面；用 uniq8 判断内容。
 */

#include "sdkconfig.h"
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"

#include "lcd_jd9366.h"
#include "camera_preview.h"

static const char *TAG = "cam_prev";

/* 官方 isp/multi_pipelines 的默认 gamma：pow(x/256, 0.7)*256，
 * 把线性 Bayer/RAW 的暗部抬起来，否则屏幕一片发暗。 */
static uint32_t cam_gamma_curve(uint32_t x)
{
    return (uint32_t)(pow((double)x / 256.0, 0.7) * 256.0);
}

/* 本板默认值：底板 V1.3，SCCB 经 U5 电平转换落到 GPIO7/8，与触摸共用 I2C0。
 * 裸显模式下触摸未初始化，这条总线只归摄像头使用。 */
#define CAM_DEF_FORMAT      "MIPI_2lane_24Minput_RAW8_800x800_30fps"
#define CAM_DEF_H_RES       800
#define CAM_DEF_V_RES       800
#define CAM_DEF_LANE_MBPS   200   /* 兜底值：仅当传感器格式未提供 mipi_clk 时才用 */
#define CAM_DEF_LANES       2
#define CAM_DEF_SDA         7
#define CAM_DEF_SCL         8
#define CAM_SCCB_HZ         (100 * 1000)
/* ISP 时钟必须高于传感器像素时钟（800x800@30fps 的 pclk 为 84 MHz），留足余量 */
#define CAM_ISP_CLK_HZ      (160 * 1000 * 1000)
/* WBG 增益归一化：G 恒为 256，R/B 由灰世界统计收敛到 R≈G≈B */
#define CAM_WBG_NORM        256
#define CAM_WBG_MIN         128
#define CAM_WBG_MAX         1023
#define CAM_AWB_SMOOTH      0.25f

typedef struct {
    int sda;
    int scl;
    const char *format_name;
    uint32_t h_res;
    uint32_t v_res;
    int lane_mbps;
    int lanes;
} cam_cfg_resolved_t;

static esp_lcd_panel_handle_t s_panel;
static void *s_fb[2];
static size_t s_frame_bytes;    /* 一帧 RGB565 字节数 */
static size_t s_fb_offset;      /* 帧在屏幕帧缓冲中的起始偏移（居中） */

static volatile int s_fb_next;  /* ping-pong 下标，在 CSI 回调里推进 */
static volatile bool s_running;
static volatile uint32_t s_frames;
static volatile uint32_t s_commits;    /* 成功提交给 DPI 的帧数 */

/* 链路健康监控：CSI host 错误标志、最近提交的帧缓冲、帧内容多样性 */
static volatile uint32_t s_csi_err_flags;
static volatile uint32_t s_csi_err_cnt;
static void *s_last_fb;

/* 传感器格式自带的 "MIPI RX init cfg"，CSI PHY 必须按它配置 */
static uint32_t s_sensor_mipi_clk_hz;
static uint32_t s_sensor_lane_num;
static uint32_t s_sensor_hs_settle;
static QueueHandle_t s_submit_q;        /* ISR -> 任务：待提交的 frame buffer */
static int s_submit_y0, s_submit_y1;   /* 摄像头画面在 fb 中的行范围 */
static int  s_single_buffer;            /* 1: 固定写 fb[0] 且不提交，用于对照 */
static int  s_awb_enable;               /* 1: 每秒按灰世界自动更新 WBG 增益 */
static float s_wb_r = CAM_WBG_NORM, s_wb_b = CAM_WBG_NORM;

static i2c_master_bus_handle_t s_i2c_bus;
static esp_sccb_io_handle_t s_sccb_io;
static esp_cam_sensor_device_t *s_sensor;
static esp_cam_ctlr_handle_t s_csi;
static isp_proc_handle_t s_isp;
static TaskHandle_t s_task;

/* ---------------- CSI 回调 ---------------- */

static bool IRAM_ATTR cam_on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    /* DPI 不会自动翻转扫描，必须由任务调用 esp_lcd_panel_draw_bitmap() 提交
     * 才会切到指定 fb；这里只需保证相邻帧写不同的 fb，避开 DPI 正在显示的那块。 */
    const int idx = s_single_buffer ? 0 : s_fb_next;
    if (!s_single_buffer) {
        s_fb_next = idx ^ 1;
    }

    trans->buffer = (uint8_t *)s_fb[idx] + s_fb_offset;
    trans->buflen = s_frame_bytes;
    return false;
}

static bool IRAM_ATTR cam_on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)user_data;
    s_frames++;

    /* 中断里只投递 buffer 指针。esp_lcd_panel_draw_bitmap() 会做 cache 写回
     * 并切换 DPI 的 cur_fb_index，必须在任务上下文调用，不能在 ISR 里执行。 */
    if (s_submit_q) {
        void *fb = trans->buffer;
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_submit_q, &fb, &woken);
        return (woken == pdTRUE);
    }
    return false;
}

static bool IRAM_ATTR cam_on_error(esp_cam_ctlr_handle_t handle, const esp_cam_ctlr_error_event_data_t *edata, void *user_data)
{
    /* 中断上下文只记标志，由 cam_rx_task 每秒汇报 */
    (void)handle;
    (void)user_data;
    if (edata) {
        s_csi_err_flags |= edata->csi_host_err_evts;
    }
    s_csi_err_cnt++;
    return false;
}

/* ---------------- 后台接收任务 ---------------- */

static void cam_rx_task(void *arg)
{
    /* 帧搬运完全由 CSI 的 DMA 完成，buffer 由 on_get_new_trans 提供；
     * esp_cam_ctlr_receive() 只在"未注册 on_get_new_trans"时才需要，
     * 这里注册了回调，再调 receive 只会把 trans 队列塞满。
     * 任务负责把写完的 fb 提交给 DPI，并周期性汇报帧数与提交数。 */
    uint32_t last = 0;
    TickType_t last_report = xTaskGetTickCount();

    while (s_running) {
        void *fb = NULL;
        if (xQueueReceive(s_submit_q, &fb, pdMS_TO_TICKS(50)) == pdTRUE) {
            /* DPI 驱动的 cur_fb_index 只在 draw_bitmap 里更新。
             * 传入落在内部 frame buffer 范围内的指针，驱动不拷贝数据，
             * 只对指定行做 cache 写回，并把显示切到这块 buffer。 */
            /* CSI/ISP 的 DMA 直接写 PSRAM，绕过 CPU cache。
             * 必须先让 CPU 侧的陈旧 cache 行失效，否则下面 draw_bitmap 内部
             * 的 C2M 写回会把旧数据盖回 fb，破坏刚写完的摄像头帧。
             * fb 起点与长度都按 cache line 对齐，M2C 不带 UNALIGNED 是合法的。 */
            if (s_single_buffer) {
                /* 单缓冲对照：DPI 恒扫 fb[0]，不做提交，只记录用于内容统计 */
                s_last_fb = fb;
                s_commits++;
            } else {
                esp_cache_msync((void *)fb, s_frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, s_submit_y0,
                                                          LCD_WIDTH, s_submit_y1, fb);
                if (err == ESP_OK) {
                    s_commits++;
                    s_last_fb = fb;
                } else {
                    ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
                }
            }
        }

        TickType_t now_tick = xTaskGetTickCount();
        if (now_tick - last_report >= pdMS_TO_TICKS(1000)) {
            const uint32_t now = s_frames;

            /* 帧内容健康指标：统计最近一帧前 16 KiB 里不同字节值的个数。
             * 真实图像通常 > 30；暗场常量约 3~5；链路无数据会更少。
             * frames 只是 DMA 定长传输完成，不能代表有画面，故保留此判据。
             * 每秒只扫 16 KiB，开销可忽略。 */
            int uniq8 = 0;
            float mr = 0.0f, mg = 0.0f, mb = 0.0f;
            uint32_t sample_n = 0;
            if (s_last_fb) {
                const size_t scan = 16 * 1024;
                esp_cache_msync(s_last_fb, scan, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                const uint8_t *p8 = (const uint8_t *)s_last_fb;
                uint8_t seen[256] = {0};
                for (size_t i = 0; i < scan; i++) {
                    seen[p8[i]] = 1;
                }
                for (int i = 0; i < 256; i++) {
                    uniq8 += seen[i];
                }

                /* RGB565 通道均值：R/B 为 5bit，G 为 6bit，统一折算到 5bit 尺度。
                 * 偏绿时 mg 明显大于 mr/mb；灰世界 AWB 据此把 R/B 增益抬上来。 */
                const uint16_t *px = (const uint16_t *)s_last_fb;
                uint32_t sr = 0, sg = 0, sb = 0;
                for (size_t i = 0; i < scan / 2; i += 4) {
                    const uint16_t v = px[i];
                    sr += (v >> 11) & 0x1f;
                    sg += (v >> 5) & 0x3f;
                    sb += v & 0x1f;
                    sample_n++;
                }
                if (sample_n) {
                    mr = (float)sr / sample_n;
                    mg = ((float)sg / sample_n) * (31.0f / 63.0f);
                    mb = (float)sb / sample_n;
                }
            }

            /* 灰世界自动白平衡：从输出端 RGB565 统计回写 WBG，令 R≈G≈B。
             * 不用 ISP AWB 控制器/统计中断，闭环简单且不占 ISR。每秒一次、带平滑。 */
            if (s_awb_enable && s_isp && sample_n && mr > 0.5f && mb > 0.5f) {
                float target_r = mg / mr * CAM_WBG_NORM;
                float target_b = mg / mb * CAM_WBG_NORM;
                target_r = target_r < CAM_WBG_MIN ? CAM_WBG_MIN : (target_r > CAM_WBG_MAX ? CAM_WBG_MAX : target_r);
                target_b = target_b < CAM_WBG_MIN ? CAM_WBG_MIN : (target_b > CAM_WBG_MAX ? CAM_WBG_MAX : target_b);
                s_wb_r += CAM_AWB_SMOOTH * (target_r - s_wb_r);
                s_wb_b += CAM_AWB_SMOOTH * (target_b - s_wb_b);
                isp_wbg_gain_t gain = {
                    .gain_r = (uint32_t)(s_wb_r + 0.5f),
                    .gain_g = CAM_WBG_NORM,
                    .gain_b = (uint32_t)(s_wb_b + 0.5f),
                };
                esp_err_t werr = esp_isp_wbg_set_wb_gain(s_isp, gain);
                if (werr != ESP_OK) {
                    ESP_LOGW(TAG, "wbg set gain failed: %s", esp_err_to_name(werr));
                }
            }

            /* rgb10 为 R/G/B 均值乘 10 的整数，便于不靠目视判断偏绿是否收敛；
             * wb 为当前 WBG 增益（G 恒 256）。 */
            ESP_LOGI(TAG, "frames=%u (+%u/s), commits=%u, uniq8=%d, rgb10=%u/%u/%u wb=%u/256/%u, csi_err=%u flags=0x%08lx",
                     (unsigned)now, (unsigned)(now - last), (unsigned)s_commits,
                     uniq8,
                     (unsigned)(mr * 10.0f), (unsigned)(mg * 10.0f), (unsigned)(mb * 10.0f),
                     (unsigned)(s_wb_r + 0.5f), (unsigned)(s_wb_b + 0.5f),
                     (unsigned)s_csi_err_cnt, (unsigned long)s_csi_err_flags);
            last = now;
            last_report = now_tick;
        }
    }
    vTaskDelete(NULL);
}

/* ---------------- 传感器 ---------------- */

static esp_err_t cam_sensor_init(const cam_cfg_resolved_t *c)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = c->sda,
        .scl_io_num = c->scl,
        .i2c_port = I2C_NUM_0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus init failed");

    esp_cam_sensor_config_t cam_cfg = {
        /* 底板 R20 已把 CSI_RST 上拉到 1.8V，传感器默认处于工作态。
         * 该脚是 1.8V 域、要求开漏，这里不复位，避免推挽输出损伤器件。 */
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
    };

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t io_cfg = {
            .scl_speed_hz = CAM_SCCB_HZ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ESP_RETURN_ON_ERROR(sccb_new_i2c_io(s_i2c_bus, &io_cfg, &cam_cfg.sccb_handle),
                            TAG, "sccb io init failed");
        cam_cfg.sensor_port = p->port;

        s_sensor = (*(p->detect))(&cam_cfg);
        if (s_sensor) {
            ESP_LOGI(TAG, "sensor detected: %s (sccb addr 0x%02x)",
                     esp_cam_sensor_get_name(s_sensor), p->sccb_addr);
            s_sccb_io = cam_cfg.sccb_handle;
            break;
        }
        esp_sccb_del_i2c_io(cam_cfg.sccb_handle);
    }

    if (!s_sensor) {
        ESP_LOGE(TAG, "no camera sensor detected on SDA=GPIO%d SCL=GPIO%d", c->sda, c->scl);
        return ESP_ERR_NOT_FOUND;
    }

    esp_cam_sensor_format_array_t fmt_array = {0};
    ESP_RETURN_ON_ERROR(esp_cam_sensor_query_format(s_sensor, &fmt_array), TAG, "query format failed");

    const esp_cam_sensor_format_t *chosen = NULL;
    for (int i = 0; i < fmt_array.count; i++) {
        ESP_LOGI(TAG, "  fmt[%d] %s", i, fmt_array.format_array[i].name);
        if (!strcmp(fmt_array.format_array[i].name, c->format_name)) {
            chosen = &fmt_array.format_array[i];
        }
    }
    if (!chosen) {
        ESP_LOGE(TAG, "format '%s' not supported by this sensor", c->format_name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_sensor_mipi_clk_hz = chosen->mipi_info.mipi_clk;
    s_sensor_lane_num    = chosen->mipi_info.lane_num;
    s_sensor_hs_settle   = chosen->mipi_info.hs_settle;
    ESP_LOGI(TAG, "sensor mipi info: clk=%u Hz (%u Mbps/lane), lanes=%u, hs_settle=%u",
             (unsigned)s_sensor_mipi_clk_hz,
             (unsigned)(s_sensor_mipi_clk_hz / 1000000),
             (unsigned)s_sensor_lane_num, (unsigned)s_sensor_hs_settle);

    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(s_sensor, chosen), TAG, "set format failed");
    ESP_LOGI(TAG, "format in use: %s", chosen->name);
    return ESP_OK;
}

/* ---------------- 对外接口 ---------------- */

esp_err_t camera_preview_start(const camera_preview_config_t *cfg)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 默认：双缓冲 ping-pong + 每帧提交（消除撕裂）；灰世界 AWB 开启。 */
    s_single_buffer = (cfg && cfg->single_buffer) ? 1 : 0;
    s_awb_enable    = (cfg && cfg->awb_disable) ? 0 : 1;
    s_wb_r = CAM_WBG_NORM;
    s_wb_b = CAM_WBG_NORM;
    ESP_LOGI(TAG, "mode: %s buffer, gray-world AWB %s",
             s_single_buffer ? "single" : "double",
             s_awb_enable ? "on" : "off");

    const cam_cfg_resolved_t c = {
        .sda = (cfg && cfg->sccb_sda_gpio > 0) ? cfg->sccb_sda_gpio : CAM_DEF_SDA,
        .scl = (cfg && cfg->sccb_scl_gpio > 0) ? cfg->sccb_scl_gpio : CAM_DEF_SCL,
        .format_name = (cfg && cfg->format_name) ? cfg->format_name : CAM_DEF_FORMAT,
        .h_res = (cfg && cfg->h_res) ? cfg->h_res : CAM_DEF_H_RES,
        .v_res = (cfg && cfg->v_res) ? cfg->v_res : CAM_DEF_V_RES,
        .lane_mbps = (cfg && cfg->lane_bit_rate_mbps > 0) ? cfg->lane_bit_rate_mbps : CAM_DEF_LANE_MBPS,
        .lanes = (cfg && cfg->data_lane_num > 0) ? cfg->data_lane_num : CAM_DEF_LANES,
    };

    s_panel = lcd_jd9366_get_panel();
    if (!s_panel) {
        ESP_LOGE(TAG, "LCD panel not initialised, call lcd_jd9366_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_fb[0], &s_fb[1]),
                        TAG, "get dpi frame buffer failed");

    s_frame_bytes = (size_t)c.h_res * c.v_res * 2;
    if (c.v_res > LCD_HEIGHT) {
        ESP_LOGE(TAG, "sensor height %u exceeds panel height %d", (unsigned)c.v_res, LCD_HEIGHT);
        return ESP_ERR_INVALID_ARG;
    }
    /* 垂直居中：上下各留 (1280 - v_res) / 2 行黑边 */
    s_fb_offset = (size_t)(LCD_HEIGHT - c.v_res) / 2 * LCD_WIDTH * 2;
    /* DPI 提交时只同步摄像头实际占用的行，避免每帧 clean 整个 2 MB fb */
    s_submit_y0 = (LCD_HEIGHT - (int)c.v_res) / 2;
    s_submit_y1 = s_submit_y0 + (int)c.v_res;

    const size_t fb_bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
    for (int i = 0; i < 2; i++) {
        memset(s_fb[i], 0x00, fb_bytes);
        ESP_RETURN_ON_ERROR(esp_cache_msync(s_fb[i], fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                            TAG, "fb cache sync failed");
    }
    ESP_LOGI(TAG, "sensor %ux%u -> fb offset %u, frame %u bytes, panel %dx%d",
             (unsigned)c.h_res, (unsigned)c.v_res, (unsigned)s_fb_offset,
             (unsigned)s_frame_bytes, LCD_WIDTH, LCD_HEIGHT);

    esp_err_t err = cam_sensor_init(&c);
    if (err != ESP_OK) {
        goto fail_sensor;
    }

    /* 传感器格式声明的 MIPI 速率优先；cfg 显式传值最高优先级，便于做速率对照。 */
    const int phy_mbps = (cfg && cfg->lane_bit_rate_mbps > 0)
                             ? c.lane_mbps
                             : (s_sensor_mipi_clk_hz
                                    ? (int)(s_sensor_mipi_clk_hz / 1000000)
                                    : c.lane_mbps);
    const int phy_lanes = (cfg && cfg->data_lane_num > 0)
                              ? c.lanes
                              : (s_sensor_lane_num ? (int)s_sensor_lane_num : c.lanes);

    /* ---- CSI：照官方 examples/peripherals/isp/multi_pipelines 配置 ----
     * SC2336 输出 RAW8（8 bpp），CSI 桥必须同样声明 RAW8 -> RAW8：
     *   - src == dst 时 s_csi_ctlr_format_conversion() 自动置 bypass，
     *     不会触发 "failed to configure format conversion"；
     *   - data_type 全留 0 -> 驱动按 color type 推出 in_bpp/out_bpp = 8，
     *     并自动把 MIPI data type 过滤区间设成 0x12~0x2F。
     * 若自定义 data_type.bits_per_pixel，驱动会用它同时覆盖 in_bpp/out_bpp，
     * 而 csi_transfer_size = h*v*in_bpp/64 决定 DMA 传输长度；设错会让 DMA
     * 期望的数据量与传感器实际不符。RAW8 -> RGB565 全部交给下面的 ISP。 */
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .h_res = c.h_res,
        .v_res = c.v_res,
        .lane_bit_rate_mbps = phy_mbps,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_lane_num = phy_lanes,
        .queue_items = 2,
        .byte_swap_en = false,
    };
    ESP_LOGI(TAG, "csi cfg: RAW8->RAW8 bypass, %ux%u, %d lanes @ %d Mbps (mipi_clk=%u Hz)",
             (unsigned)c.h_res, (unsigned)c.v_res, phy_lanes, phy_mbps,
             (unsigned)s_sensor_mipi_clk_hz);
    err = esp_cam_new_csi_ctlr(&csi_cfg, &s_csi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi init failed: %s", esp_err_to_name(err));
        goto fail_sensor;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = cam_on_get_new_trans,
        .on_trans_finished = cam_on_trans_finished,
        .on_error = cam_on_error,
    };
    err = esp_cam_ctlr_register_event_callbacks(s_csi, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi callback register failed: %s", esp_err_to_name(err));
        goto fail_csi;
    }
    err = esp_cam_ctlr_enable(s_csi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi enable failed: %s", esp_err_to_name(err));
        goto fail_csi;
    }

    /* ---- ISP：RAW8 -> RGB565 ---- */
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz = CAM_ISP_CLK_HZ,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = c.h_res,
        .v_res = c.v_res,
        /* SC2336 是 BGGR 排列，与 ISP 默认值一致，显式写出避免依赖隐式默认 */
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
    };
    err = esp_isp_new_processor(&isp_cfg, &s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp init failed: %s", esp_err_to_name(err));
        goto fail_csi_enabled;
    }
    err = esp_isp_enable(s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp enable failed: %s", esp_err_to_name(err));
        goto fail_isp;
    }

    /* ---- WBG：Bayer 域白平衡增益 ----
     * Bayer 阵列 G 采样点是 R/B 的两倍，不做 WBG 输出必然偏绿。
     * 这里不接 ISP AWB 控制器，改由 cam_rx_task 按输出 RGB565 做灰世界闭环。 */
    esp_isp_wbg_config_t wbg_cfg = {
        .flags = { .update_once_configured = true },
    };
    err = esp_isp_wbg_configure(s_isp, &wbg_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp wbg configure failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_wbg_enable(s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp wbg enable failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    isp_wbg_gain_t wb_init = {
        .gain_r = CAM_WBG_NORM, .gain_g = CAM_WBG_NORM, .gain_b = CAM_WBG_NORM,
    };
    err = esp_isp_wbg_set_wb_gain(s_isp, wb_init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp wbg set gain failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    ESP_LOGI(TAG, "isp wbg enabled (neutral 256/256/256, gray-world loop)");

    /* RAW8 -> RGB565 必须显式配置并使能 demosaic。
     * 只 new_processor + enable 时 ISP 输出的是未去马赛克的 Bayer/填充数据，
     * 屏幕上就是错位条纹而非图像。参数照抄官方 isp/multi_pipelines 示例。 */
    esp_isp_demosaic_config_t demosaic_cfg = {
        .grad_ratio = { .integer = 2, .decimal = 5 },
    };
    err = esp_isp_demosaic_configure(s_isp, &demosaic_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp demosaic configure failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_demosaic_enable(s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp demosaic enable failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    ESP_LOGI(TAG, "isp demosaic enabled (grad_ratio 2.5)");

    /* ---- gamma：把线性数据的暗部抬起来（官方示例 pow(x/256,0.7)*256） ---- */
    isp_gamma_curve_points_t gamma_pts = {};
    err = esp_isp_gamma_fill_curve_points(cam_gamma_curve, &gamma_pts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp gamma curve failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_gamma_configure(s_isp, COLOR_COMPONENT_R, &gamma_pts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp gamma configure R failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_gamma_configure(s_isp, COLOR_COMPONENT_G, &gamma_pts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp gamma configure G failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_gamma_configure(s_isp, COLOR_COMPONENT_B, &gamma_pts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp gamma configure B failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_gamma_enable(s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp gamma enable failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    ESP_LOGI(TAG, "isp gamma enabled (0.7)");

    /* ---- color：对比度 / 饱和度保持中性，补齐全链路 ---- */
    esp_isp_color_config_t color_cfg = {
        .color_contrast   = { .integer = 1, .decimal = 0 },
        .color_saturation = { .integer = 1, .decimal = 0 },
        .color_hue = 0,
        .color_brightness = 0,
    };
    err = esp_isp_color_configure(s_isp, &color_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp color configure failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    err = esp_isp_color_enable(s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isp color enable failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }
    ESP_LOGI(TAG, "isp color enabled (contrast 1.0, saturation 1.0)");

    /* 双缓冲 ping-pong 从 fb[0] 开始 */
    s_fb_next = 0;
    s_frames = 0;
    s_commits = 0;
    s_csi_err_flags = 0;
    s_csi_err_cnt = 0;
    s_last_fb = NULL;

    s_submit_q = xQueueCreate(3, sizeof(void *));
    if (!s_submit_q) {
        err = ESP_ERR_NO_MEM;
        goto fail_isp_enabled;
    }

    /* 与官方 mipi_isp_dsi 示例一致：传感器先开流，再启动 CSI/DMA。
     * CSI 就绪时数据已经在路上，避免 start 时桥空转。 */
    int stream_on = 1;
    err = esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor stream on failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
    }

    /* 可选：让传感器自己输出内部灰度渐变测试图案，用于独立验证
     * sensor TX -> MIPI -> CSI -> ISP -> DMA 整条链路。cfg=NULL 时关闭，
     * 正常成像。SC2336 实现为写 0x4501 的 bit3。 */
    if (cfg && cfg->test_pattern) {
        int test_pat = 1;
        err = esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_TEST_PATTERN, &test_pat);
        ESP_LOGI(TAG, "sensor test pattern on: %s", esp_err_to_name(err));
        if (err != ESP_OK) {
            goto fail_stream;
        }
    }

    s_running = true;

    err = esp_cam_ctlr_start(s_csi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi start failed: %s", esp_err_to_name(err));
        s_running = false;
        goto fail_stream;
    }

    if (xTaskCreate(cam_rx_task, "cam_preview", 4096, NULL, 5, &s_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail_stream;
    }

    ESP_LOGI(TAG, "camera preview started");
    return ESP_OK;

fail_stream: {
        int stream_off = 0;
        s_running = false;
        if (s_csi) {
            esp_cam_ctlr_stop(s_csi);
        }
        if (s_sensor) {
            esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_off);
        }
    }
fail_isp_enabled:
    esp_isp_disable(s_isp);
fail_isp:
    esp_isp_del_processor(s_isp);
    s_isp = NULL;
fail_csi_enabled:
    esp_cam_ctlr_disable(s_csi);
fail_csi:
    esp_cam_ctlr_del(s_csi);
    s_csi = NULL;
fail_sensor:
    return err;
}

esp_err_t camera_preview_stop(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_running = false;
    if (s_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_task = NULL;
    }

    int stream_off = 0;
    if (s_sensor) {
        esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_off);
    }
    if (s_csi) {
        esp_cam_ctlr_stop(s_csi);
        esp_cam_ctlr_disable(s_csi);
        esp_cam_ctlr_del(s_csi);
        s_csi = NULL;
    }
    if (s_submit_q) {
        vQueueDelete(s_submit_q);
        s_submit_q = NULL;
    }
    if (s_isp) {
        esp_isp_disable(s_isp);
        esp_isp_del_processor(s_isp);
        s_isp = NULL;
    }
    if (s_sensor) {
        esp_cam_sensor_del_dev(s_sensor);
        s_sensor = NULL;
    }
    if (s_sccb_io) {
        esp_sccb_del_i2c_io(s_sccb_io);
        s_sccb_io = NULL;
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    ESP_LOGI(TAG, "camera preview stopped");
    return ESP_OK;
}

uint32_t camera_preview_get_frame_count(void)
{
    return s_frames;
}
