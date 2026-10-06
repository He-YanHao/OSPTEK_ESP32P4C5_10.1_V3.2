#ifndef ESP_AT_SPI_H
#define ESP_AT_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 *  ESP-AT SPI 协议层
 * ============================================================
 *
 * 【定位】
 *   封装 ESP-AT 的 SPI 从机协议，向上提供「读写数据包」语义。
 *   向下依赖 hal_spi.h 提供的物理 SPI 事务。
 *
 * 【跨平台】
 *   本层不依赖任何 ESP-IDF 特有接口（除 HAL 调用），
 *   移植时与 app_bridge.c 一起直接复制。
 *
 * 【协议概述】
 *   C5 作为 SPI 从机，通过 IRQ 主动通知主机 P4。
 *   每次事务格式：[CMD][ADDR][DUMMY] + DATA
 *   关键命令：
 *     WRBUF (0x01) : P4 通知 C5 "我要写 N 字节"
 *     RDBUF (0x02) : P4 查询 C5 当前状态（direct + len）
 *     WRDMA (0x03) : P4 写实际数据
 *     RDDMA (0x04) : P4 读实际数据
 *     WR_END(0x07) : P4 通知 C5 写完成
 *     INT0  (0x08) : P4 通知 C5 读完成
 *
 * 【上下文约束】
 *   所有 read/write 必须从同一 FreeRTOS 任务调用。
 *   内部用 s_bus_mutex 串行化 SPI 事务，但状态机假设单调用者。
 */

/* ---------- 协议命令码 ---------- */
#define CMD_HD_WRBUF_REG    0x01
#define CMD_HD_RDBUF_REG    0x02
#define CMD_HD_WRDMA_REG    0x03
#define CMD_HD_RDDMA_REG    0x04
#define CMD_HD_WR_END_REG   0x07
#define CMD_HD_INT0_REG     0x08

/* ---------- 寄存器地址 ---------- */
#define WRBUF_START_ADDR    0x00
#define RDBUF_START_ADDR    0x04

/* ---------- direct 字段值（C5 当前允许的方向） ---------- */
#define SPI_DIR_NULL        0x00   /* 空闲 */
#define SPI_DIR_READ        0x01   /* C5 有数据要发给 P4 */
#define SPI_DIR_WRITE       0x02   /* C5 准备好接收 P4 的数据 */

/* ---------- 协议常量 ---------- */
#define SPI_SEND_MAGIC      0xFE       /* WRBUF 包里的魔数，防止误触发 */
#define SPI_AT_MAX_PAYLOAD  4092       /* 单次 SPI 事务最大 payload */

/* ---------- 协议结构体（小端，packed） ---------- */

/* P4 → C5：写请求（WRBUF 的 payload） */
typedef struct {
    uint8_t  magic;      /* 固定 0xFE */
    uint8_t  send_seq;   /* 序号，每次递增 */
    uint16_t send_len;   /* 本次要写的字节数 */
} __attribute__((packed)) spi_send_opt_t;

/* C5 → P4：状态回包（RDBUF 的 payload） */
typedef struct {
    uint8_t  direct;       /* 方向：0=空闲 1=可读 2=可写 */
    uint8_t  seq_num;      /* C5 当前包的序号 */
    uint16_t transmit_len; /* 本包长度 */
} __attribute__((packed)) spi_recv_opt_t;

/* ---------- API ---------- */

/* 初始化协议层（内部调 hal_spi_init + 创建互斥量） */
int esp_at_spi_init(void);

/* 复位 C5 并等 ready 脉冲（5 秒超时） */
int esp_at_spi_reset_and_wait_ready(uint32_t timeout_ms);

/* 转发等 IRQ（供 app_bridge 使用，避免直接依赖 hal_spi） */
int esp_at_spi_wait_irq(uint32_t timeout_ms);

/*
 * 写数据到 C5（完整握手流程）
 *   流程：WRBUF → 等 IRQ → RDBUF 确认可写 → WRDMA(send_len) → WR_END
 * 返回：>=0 实际写入字节数（可 < len）；<0 参数错误
 */
int esp_at_spi_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/*
 * 从 C5 读一包数据（完整握手流程）
 *   流程：RDBUF → seq 去重 → RDDMA → INT0
 * 返回：>0 本次读入字节数；0 无数据；<0 错误
 */
int esp_at_spi_read(uint8_t *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AT_SPI_H */