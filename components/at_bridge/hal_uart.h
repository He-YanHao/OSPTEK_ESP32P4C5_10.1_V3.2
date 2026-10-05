#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HAL_UART —— 数据消费层
 *
 * 当前实现：UART 输出到 PC（调试）
 * 未来替换：直接写 Flash/SD/其他存储
 *
 * 语义契约（实现方必须遵守）：
 *   hal_uart_init  : 初始化，幂等；0=成功，<0=失败
 *   hal_uart_read  : 从上游读数据
 *                    >0 实际读到的字节数
 *                    =0 超时无数据
 *                    <0 错误
 *   hal_uart_write : 写入数据到消费者
 *                    = len 全部写完
 *                    < len 部分写完（返回实际写入字节数）
 *                    <0 错误
 *
 * 实现方须保证：
 *   - write 不主动丢弃数据。缓冲满时阻塞等待，不丢弃、不忙等。
 *   - 长时间阻塞须 vTaskDelay 让出 CPU，避免看门狗。
 *   - 若上层传入的数据超过一次可写，返回实际写入字节数，
 *     由上层保留剩余部分重试。
 */

int hal_uart_init(void);
int hal_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
int hal_uart_write(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UART_H */