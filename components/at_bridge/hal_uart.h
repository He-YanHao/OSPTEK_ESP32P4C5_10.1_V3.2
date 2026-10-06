#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 *  HAL_UART —— 数据消费层
 * ============================================================
 *
 * 【定位】
 *   本层是桥接链路的最下游。P4 从 C5 收到的所有数据最终交给这里消费。
 *
 * 【当前实现】
 *   UART 输出到 PC（调试用）
 *
 * 【未来替换】
 *   直接写 Flash / SD 卡 / 其他存储介质 —— 只需重写 hal_uart.c
 *
 * 【语义契约】（实现方必须遵守，否则 app_bridge 的"不丢数据"承诺破裂）
 *
 *   hal_uart_init  : 初始化，幂等
 *                    返回 0 表示成功，<0 表示失败
 *                    重复调用必须返回 0，不能重复初始化底层硬件
 *
 *   hal_uart_read  : 从上游读数据（只在 app_bridge 上行路径调用）
 *                    返回 >0 实际读到的字节数
 *                    返回 =0 超时无数据
 *                    返回 <0 错误
 *
 *   hal_uart_write : 写入数据到消费者
 *                    返回 = len  全部写完
 *                    返回 < len  部分写完（返回实际写入字节数）
 *                    返回 < 0    无法写入
 *
 * 【实现方必须保证】
 *   1. write 不主动丢弃数据。缓冲满时阻塞等待，不丢弃、不忙等。
 *   2. 长时间阻塞须 vTaskDelay 让出 CPU，避免看门狗复位。
 *   3. 若上层传入的数据超过一次可写容量，返回实际写入字节数，
 *      由上层（app_bridge）保留剩余部分重试。
 *   4. 一旦返回 <0，上层会视为设备故障并停止读取，谨慎返回。
 */

int hal_uart_init(void);
int hal_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
int hal_uart_write(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UART_H */