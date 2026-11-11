#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动轻量 RTSP Server（单客户端，H.264 over RTP/UDP）。
 *
 * 只依赖 lwIP socket，不引入 esp-adf / gmf / media_service 生态。
 * 监听 0.0.0.0:port，支持 OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN。
 * 会话由 rtsp_server_push_frame() 喂入 Annex-B H.264 帧，
 * 内部按起始码切 NAL 并做 single-NAL / FU-A 分包。
 */
esp_err_t rtsp_server_start(uint16_t port);

/** @brief 送入一帧 Annex-B 码流（可含 SPS/PPS/IDR 多个 NAL）。非阻塞，队列满即丢帧。 */
void rtsp_server_push_frame(const uint8_t *annexb, size_t len, bool is_idr);

/** @brief 统计：累计会话数 / RTP 包数 / RTP 字节数 / 丢帧数 */
void rtsp_server_get_stats(uint32_t *sessions, uint32_t *packets,
                            uint32_t *bytes, uint32_t *dropped);

#ifdef __cplusplus
}
#endif
