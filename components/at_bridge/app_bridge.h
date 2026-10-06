#ifndef APP_BRIDGE_H
#define APP_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 *  app_bridge —— 双向字节桥接层
 * ============================================================
 *
 * 【定位】
 *   桥接 PC ↔ P4 ↔ C5 三方的数据流。
 *   P4 不解析任何 AT 命令，只做纯字节搬运。
 *
 * 【单例说明】
 *   本模块所有任务/缓冲区均为单例，
 *   进程内只允许一次 app_bridge_start 调用。
 *
 * 【数据流】
 *   上行（PC → C5）：UART RX → esp_at_spi_write → C5
 *   下行（C5 → PC）：C5 → esp_at_spi_read → StreamBuffer → UART TX
 *
 * 【流控】
 *   背压而非丢弃：
 *     - StreamBuffer 有空间 → P4 读 C5
 *     - StreamBuffer 无空间 → P4 停止读 → C5 buffer 满 → TCP 背压
 *
 * 【可移植性】
 *   本文件只依赖 FreeRTOS + hal_uart.h + esp_at_spi.h，
 *   移植时与其他跨平台文件一起复制即可。
 */

/* UART 单次处理块大小（影响任务栈需求） */
#ifndef APP_BRIDGE_UART_CHUNK
#define APP_BRIDGE_UART_CHUNK   1024
#endif

/* SPI 单次读上限（不能超过协议层 SPI_AT_MAX_PAYLOAD） */
#ifndef APP_BRIDGE_SPI_CHUNK
#define APP_BRIDGE_SPI_CHUNK    4092
#endif

/* StreamBuffer 容量
 * 32KB 依据：UART 460800 ≈ 46KB/s，SPI 400kHz ≈ 50KB/s，
 * 32KB 可吸收约 0.7 秒的速度差抖动。 */
#ifndef APP_BRIDGE_SB_SIZE
#define APP_BRIDGE_SB_SIZE      (32 * 1024)
#endif

/*
 * 启动桥接。
 * 内部会：
 *   1. hal_uart_init() + esp_at_spi_init()
 *   2. 创建 StreamBuffer
 *   3. 创建 uart_tx_task 和 spi_task
 * 返回：0 成功，-1 失败（详细错误看日志）
 */
int app_bridge_start(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BRIDGE_H */