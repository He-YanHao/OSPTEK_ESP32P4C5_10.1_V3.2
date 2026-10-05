#ifndef HAL_SPI_H
#define HAL_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件层只做四件事：
 *   1. init
 *   2. 等 IRQ
 *   3. 一次 SPI 事务（3 字节头 + payload）
 *   4. 清 IRQ 计数 / 控制 C5 电源
 * 不了解 WRBUF / RDBUF / seq / 包。 */

int  hal_spi_init(void);
int  hal_spi_wait_irq(uint32_t timeout_ms);
int  hal_spi_transfer(uint8_t cmd, uint8_t addr,
                      const uint8_t *tx, uint8_t *rx, size_t len);
void hal_spi_clear_irq(void);

void hal_c5_power_on(void);
void hal_c5_power_off(void);
void hal_c5_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SPI_H */