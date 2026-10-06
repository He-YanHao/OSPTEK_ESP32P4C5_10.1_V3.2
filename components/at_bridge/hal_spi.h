#ifndef HAL_SPI_H
#define HAL_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 *  HAL_SPI —— SPI 硬件层
 * ============================================================
 *
 * 【定位】
 *   本层只负责「物理 SPI 事务」和「GPIO 中断」，
 *   完全不理解 WRBUF / RDBUF / seq / 包 等协议语义。
 *
 * 【职责范围】
 *   1. init          : 初始化 SPI 主机、GPIO 中断、C5 电源引脚
 *   2. wait_irq      : 等 C5 拉高 IRQ（通知 P4 有事件）
 *   3. transfer      : 执行一次 SPI 事务（3 字节头 + payload）
 *   4. clear_irq     : 清空 IRQ 计数（复位后使用）
 *   5. power / reset : 控制 C5 电源
 *
 * 【事务格式】（供协议层参考，本层不解析）
 *   [CMD][ADDR][0xFF] + payload
 *   每次 CS 拉低完整传完，不做多次事务。
 *
 * 【移植指南】
 *   移植到其他 MCU 时只需重写 hal_spi.c，
 *   上层 esp_at_spi.c / app_bridge.c 完全不动。
 */

int  hal_spi_init(void);

/*
 * 等 C5 拉高 IRQ（上升沿中断）。
 * timeout_ms = 0 表示非阻塞（立即返回）。
 * 返回：0 收到；<0 超时
 */
int  hal_spi_wait_irq(uint32_t timeout_ms);

/*
 * 单次 SPI 事务。
 * cmd/addr 放在 3 字节头里，tx 为要发的 payload，rx 为接收缓冲。
 * tx==NULL 表示只读（MOSI 输出 0xFF），rx==NULL 表示只写。
 * 返回：0 成功；<0 失败
 */
int  hal_spi_transfer(uint8_t cmd, uint8_t addr,
                      const uint8_t *tx, uint8_t *rx, size_t len);

/* 清空 IRQ 计数（复位后调用，避免旧信号残留） */
void hal_spi_clear_irq(void);

/* C5 电源控制：C5CHIP_PU 引脚 */
void hal_c5_power_on(void);
void hal_c5_power_off(void);

/* C5 复位：拉低 PU 100ms 再拉高，然后清 IRQ */
void hal_c5_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SPI_H */