#ifndef HAL_UART_H
#define HAL_UART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int hal_uart_init(void);
int hal_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
int hal_uart_write(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UART_H */