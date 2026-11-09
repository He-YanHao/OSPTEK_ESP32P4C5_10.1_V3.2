/*
 * camera_preview —— SC2336 -> MIPI-CSI -> ISP -> DSI 帧缓冲，零拷贝直显
 *
 * 参考 ESP-IDF examples/peripherals/camera/mipi_isp_dsi，针对本板做了三处调整：
 *   1) 传感器输出 800x800（SC2336 唯一与屏幕同宽的格式），垂直居中放进 800x1280 屏；
 *   2) CSI 接收缓冲直接用 DPI 内部帧缓冲的中间区域，不做 CPU 搬运、不用 PPA；
 *   3) 两个 DPI 帧缓冲 ping-pong，避免 DPI 双缓冲扫描时出现黑帧闪烁。
 */

#include "sdkconfig.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* 本板默认值：底板 V1.3，SCCB 经 U5 电平转换落到 GPIO7/8，与触摸共用 I2C0。
 * 裸显模式下触摸未初始化，这条总线只归摄像头使用。 */
#define CAM_DEF_FORMAT      "MIPI_2lane_24Minput_RAW8_800x800_30fps"
#define CAM_DEF_H_RES       800
#define CAM_DEF_V_RES       800
#define CAM_DEF_LANE_MBPS   200   /* 与官方 mipi_isp_dsi 示例一致；D-PHY settle 对偏大的值更敏感 */
#define CAM_DEF_LANES       2
#define CAM_DEF_SDA         7
#define CAM_DEF_SCL         8
#define CAM_SCCB_HZ         (100 * 1000)
/* ISP 时钟必须高于传感器像素时钟（800x800@30fps 的 pclk 为 84 MHz），留足余量 */
#define CAM_ISP_CLK_HZ      (160 * 1000 * 1000)

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

static i2c_master_bus_handle_t s_i2c_bus;
static esp_sccb_io_handle_t s_sccb_io;
static esp_cam_sensor_device_t *s_sensor;
static esp_cam_ctlr_handle_t s_csi;
static isp_proc_handle_t s_isp;
static TaskHandle_t s_task;

/* ---------------- CSI 回调 ---------------- */

static bool IRAM_ATTR cam_on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    /* DPI 在 fb[0] / fb[1] 之间自动翻转扫描，摄像头帧必须交替写这两个缓冲，
     * 否则会出现一帧有画面、一帧全黑的闪烁。 */
    const int idx = s_fb_next;
    s_fb_next = idx ^ 1;

    trans->buffer = (uint8_t *)s_fb[idx] + s_fb_offset;
    trans->buflen = s_frame_bytes;
    return false;
}

static bool IRAM_ATTR cam_on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    s_frames++;
    return false;
}

static bool IRAM_ATTR cam_on_error(esp_cam_ctlr_handle_t handle, const esp_cam_ctlr_error_event_data_t *edata, void *user_data)
{
    /* 中断上下文只能记标志，不能打日志 */
    (void)handle;
    (void)edata;
    (void)user_data;
    return false;
}

/* ---------------- 后台接收任务 ---------------- */

static void cam_rx_task(void *arg)
{
    /* 帧搬运完全由 CSI 的 DMA 完成，buffer 由 on_get_new_trans 提供；
     * esp_cam_ctlr_receive() 只在"未注册 on_get_new_trans"时才需要，
     * 这里注册了回调，再调 receive 只会把 trans 队列塞满。
     * 任务只负责周期性汇报帧数，便于判断链路是否在动。 */
    uint32_t last = 0;

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t now = s_frames;
        ESP_LOGI(TAG, "frames=%u (+%u/s)", (unsigned)now, (unsigned)(now - last));
        last = now;
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

    /* ---- CSI ---- */
    /* 注意：不能把 RAW8 -> RGB565 交给 CSI 的桥内颜色转换。
     * esp_cam_ctlr_csi.c 的 s_is_color_format_conversion_supported() 只认
     * RGB888/RGB565/YUV4xx，RAW8 不在白名单里（会对 RAW8 直接返回
     * ESP_ERR_NOT_SUPPORTED）。正确做法是让 CSI 走 bypass（只做 PHY->DMA 搬运），
     * 由 ISP 完成 RAW8 -> RGB565；把 data_type.bits_per_pixel 设成 16 既能触发
     * bypass（ctlr->custom_data_depth 非 0），又让帧长按 RGB565 计算。
     * data_type_min/max 必须显式给 RAW8 的 MIPI data type 0x2A，否则桥收不到包。 */
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .h_res = c.h_res,
        .v_res = c.v_res,
        .lane_bit_rate_mbps = c.lane_mbps,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_type = {
            .bits_per_pixel = 16,
            /* 用标准 MIPI CSI-2 数据类型区间（0x12~0x2F，覆盖 RAW8/RAW10/RGB/YUV），
             * 只锁 0x2A 会在传感器实际发 RAW10 时把整帧滤掉。 */
            .data_type_min = 0x12,
            .data_type_max = 0x2F,
        },
        .data_lane_num = c.lanes,
        .queue_items = 2,
        .byte_swap_en = false,
    };
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

    /* 双缓冲 ping-pong 从 fb[0] 开始 */
    s_fb_next = 0;
    s_frames = 0;

    /* 与官方 mipi_isp_dsi 示例一致：传感器先开流，再启动 CSI/DMA。
     * CSI 就绪时数据已经在路上，避免 start 时桥空转。 */
    int stream_on = 1;
    err = esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor stream on failed: %s", esp_err_to_name(err));
        goto fail_isp_enabled;
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
        vTaskDelay(pdMS_TO_TICKS(50));
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
