#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 H.264 硬件编码器并注册帧回调。
 *
 * 必须在 camera_preview_start() 之前调用，否则首帧回调会漏掉。
 * 输入为 camera_preview 输出的 800x800 RGB565_LE，编码器输出 Annex-B NAL，
 * 每帧交给 rtsp_server_push_frame() 由 RTSP Server 推送（非阻塞）。
 */
esp_err_t h264_sink_start(void);

/** @brief 读取编码统计（累计帧数 / 字节数 / IDR 数 / 错误数） */
void h264_sink_get_stats(uint32_t *frames, uint32_t *bytes,
                         uint32_t *idr, uint32_t *errors);

#ifdef __cplusplus
}
#endif
