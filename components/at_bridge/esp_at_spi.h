#ifndef ESP_AT_SPI_H
#define ESP_AT_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-AT SPI 协议码 */
#define CMD_HD_WRBUF_REG    0x01
#define CMD_HD_RDBUF_REG    0x02
#define CMD_HD_WRDMA_REG    0x03
#define CMD_HD_RDDMA_REG    0x04
#define CMD_HD_WR_END_REG   0x07
#define CMD_HD_INT0_REG     0x08

#define WRBUF_START_ADDR    0x00
#define RDBUF_START_ADDR    0x04

/* direct 字段值 */
#define SPI_DIR_NULL        0x00
#define SPI_DIR_READ        0x01
#define SPI_DIR_WRITE       0x02

#define SPI_SEND_MAGIC      0xFE
#define SPI_AT_MAX_PAYLOAD  4092

/* 协议包结构（小端） */
typedef struct {
    uint8_t  magic;
    uint8_t  send_seq;
    uint16_t send_len;
} __attribute__((packed)) spi_send_opt_t;

typedef struct {
    uint8_t  direct;
    uint8_t  seq_num;
    uint16_t transmit_len;
} __attribute__((packed)) spi_recv_opt_t;

/*
 * 上下文约束：
 *   所有 read/write 必须从同一 FreeRTOS 任务调用
 *   （内部用 s_bus_mutex 串行化 SPI 事务，但状态机假设单调用者）
 */

int esp_at_spi_init(void);
int esp_at_spi_reset_and_wait_ready(uint32_t timeout_ms);
int esp_at_spi_wait_irq(uint32_t timeout_ms);
int esp_at_spi_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
int esp_at_spi_read(uint8_t *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AT_SPI_H */