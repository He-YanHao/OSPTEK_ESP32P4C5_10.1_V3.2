#ifndef APP_BRIDGE_H
#define APP_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_BRIDGE_UART_CHUNK
#define APP_BRIDGE_UART_CHUNK   1024
#endif

#ifndef APP_BRIDGE_SPI_CHUNK
#define APP_BRIDGE_SPI_CHUNK    4092
#endif

#ifndef APP_BRIDGE_SB_SIZE
#define APP_BRIDGE_SB_SIZE      (64 * 1024)
#endif

int app_bridge_start(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BRIDGE_H */