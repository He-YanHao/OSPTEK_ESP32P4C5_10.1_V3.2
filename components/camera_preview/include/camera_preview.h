/*
 * camera_preview —— 摄像头本地直显（不依赖 LVGL）
 *
 * 数据通路（零拷贝）：
 *   SC2336 RAW8 --MIPI-CSI--> ESP32-P4 ISP --RGB565 DMA--> DSI framebuffer
 *
 * 摄像头帧由 CSI/ISP 直接写进 DPI 面板的内部帧缓冲，CPU 不参与搬运。
 * 采用双帧缓冲 ping-pong，与 DPI 的双缓冲扫描对应。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sccb_sda_gpio;        /*!< SCCB SDA，本板 GPIO7 */
    int sccb_scl_gpio;        /*!< SCCB SCL，本板 GPIO8 */
    const char *format_name;  /*!< 传感器输出格式名，NULL 用默认 800x800@30fps */
    uint32_t h_res;           /*!< 传感器输出宽度，0 用默认 800 */
    uint32_t v_res;           /*!< 传感器输出高度，0 用默认 800 */
    int lane_bit_rate_mbps;   /*!< MIPI 每 lane 速率，0 用默认 336 */
    int data_lane_num;        /*!< MIPI lane 数，0 用默认 2 */
} camera_preview_config_t;

/**
 * @brief 启动摄像头直显。
 *
 * 调用前必须已经完成 lcd_jd9366_init()。函数内部会创建后台采集任务后立即返回，
 * 画面持续刷新由该任务驱动。
 *
 * @param cfg 配置，传 NULL 使用本板默认值
 * @return ESP_OK 启动成功；其余为具体失败原因（传感器未找到、CSI/ISP 初始化失败等）
 */
esp_err_t camera_preview_start(const camera_preview_config_t *cfg);

/**
 * @brief 停止采集并释放资源。可再次调用 camera_preview_start() 重启。
 */
esp_err_t camera_preview_stop(void);

/**
 * @brief 累计收到的完整帧数，用于观察帧率/判断是否卡死。
 */
uint32_t camera_preview_get_frame_count(void);

#ifdef __cplusplus
}
#endif
