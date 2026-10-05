#include "esp_at_spi.h"
#include "hal_spi.h"
#include "freertos_port.h"
#include <string.h>

/* 日志：默认静默；平台可定义 AT_LOG(fmt, ...) */
#ifndef AT_LOG
#define AT_LOG(fmt, ...)  do {} while (0)
#endif

static SemaphoreHandle_t s_bus_mutex = NULL;
static uint8_t           s_send_seq  = 0;
static uint8_t           s_last_consumed_seq = 0xFF;   /* ★ 恢复 seq 去重 */
static int               s_inited = 0;

typedef struct {
    uint8_t  seq;
    uint16_t total_len;
    uint16_t read_len;
    bool     in_progress;
} pkt_state_t;

static pkt_state_t s_pkt = { .in_progress = false };

static int at_spi_xfer(uint8_t cmd, uint8_t addr,
                       const uint8_t *tx, uint8_t *rx, size_t len)
{
    return hal_spi_transfer(cmd, addr, tx, rx, len);
}

static int at_query_slave(spi_recv_opt_t *out)
{
    return at_spi_xfer(CMD_HD_RDBUF_REG, RDBUF_START_ADDR,
                       NULL, (uint8_t *)out, sizeof(*out));
}

static int at_request_write(uint8_t seq, uint16_t len)
{
    spi_send_opt_t opt = {
        .magic    = SPI_SEND_MAGIC,
        .send_seq = seq,
        .send_len = len,
    };
    return at_spi_xfer(CMD_HD_WRBUF_REG, WRBUF_START_ADDR,
                       (const uint8_t *)&opt, NULL, sizeof(opt));
}

int esp_at_spi_init(void)
{
    if (s_inited) return 0;
    if (hal_spi_init() != 0) return -1;

    s_bus_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex) return -1;

    s_send_seq = 0;
    s_last_consumed_seq = 0xFF;
    s_pkt.in_progress = false;
    s_inited = 1;
    return 0;
}

int esp_at_spi_reset_and_wait_ready(uint32_t timeout_ms)
{
    hal_c5_reset();
    s_pkt.in_progress = false;
    s_send_seq = 0;
    s_last_consumed_seq = 0xFF;
    return hal_spi_wait_irq(timeout_ms);
}

int esp_at_spi_wait_irq(uint32_t timeout_ms)
{
    return hal_spi_wait_irq(timeout_ms);
}

/* ============================================================
 * 写：WRBUF → 轮询 RDBUF(取 transmit_len) → WRDMA(send_len) → WR_END
 *
 * ★ 不再调用 hal_spi_clear_irq()。那会误杀 C5 已就绪的响应 IRQ。
 * ★ send_len 受 transmit_len 约束，防止 C5 静默丢弃多余字节。
 * ============================================================ */
int esp_at_spi_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!data || !len) return -1;
    if (xSemaphoreTake(s_bus_mutex, portMAX_DELAY) != pdTRUE) return -1;

    size_t done = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t to    = pdMS_TO_TICKS(timeout_ms);

    while (done < len) {
        if ((xTaskGetTickCount() - start) >= to) break;

        size_t chunk = len - done;
        if (chunk > SPI_AT_MAX_PAYLOAD) chunk = SPI_AT_MAX_PAYLOAD;

        s_send_seq = (s_send_seq + 1) & 0xFF;
        if (at_request_write(s_send_seq, (uint16_t)chunk) != 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        bool   writable  = false;
        size_t max_write = 0;
        TickType_t poll_start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - poll_start) < pdMS_TO_TICKS(20)) {
            spi_recv_opt_t opt;
            if (at_query_slave(&opt) == 0) {
                if (opt.direct == SPI_DIR_WRITE) {
                    writable  = true;
                    max_write = opt.transmit_len;
                    break;
                }
                if (opt.direct == SPI_DIR_READ) break;
            }
            /* 主动让锁，让读路径有机会处理 C5 的响应 */
            xSemaphoreGive(s_bus_mutex);
            vTaskDelay(pdMS_TO_TICKS(2));
            if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                return -1;
            }
        }
        if (!writable) break;

        size_t send_len = (chunk < max_write) ? chunk : max_write;
        if (send_len == 0 || send_len > SPI_AT_MAX_PAYLOAD) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (at_spi_xfer(CMD_HD_WRDMA_REG, 0x00, data + done, NULL, send_len) != 0) break;
        if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) break;

        done += send_len;
    }

    xSemaphoreGive(s_bus_mutex);
    return (int)done;
}

/* ============================================================
 * 读：RDBUF → RDDMA(分段) → INT0
 *
 * ★ 恢复 seq 去重：整包读完 + INT0 成功后才更新 s_last_consumed_seq。
 *   若 RDBUF 返回相同 seq，视为"该包已消费"，返回 0。
 *   这避免了"同一包反复读"（38 字节刷屏）。
 * ============================================================ */
int esp_at_spi_read(uint8_t *buf, size_t buf_size)
{
    if (!buf || !buf_size) return 0;
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;

    int ret = 0;
    bool error = false;

    if (!s_pkt.in_progress) {
        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) {
            goto done;   /* SPI 抖动，返回 0（无数据），不当错误 */
        }
        if (opt.direct != SPI_DIR_READ || opt.transmit_len == 0) {
            goto done;
        }

        /* ★ seq 去重：已消费过的包不再读 */
        if (opt.seq_num == s_last_consumed_seq) {
            goto done;
        }

        s_pkt.seq         = opt.seq_num;
        s_pkt.total_len   = opt.transmit_len;
        s_pkt.read_len    = 0;
        s_pkt.in_progress = true;
    }

    size_t remain  = s_pkt.total_len - s_pkt.read_len;
    size_t to_read = (remain < buf_size) ? remain : buf_size;

    size_t done = 0;
    while (done < to_read) {
        size_t c = to_read - done;
        if (c > SPI_AT_MAX_PAYLOAD) c = SPI_AT_MAX_PAYLOAD;
        if (at_spi_xfer(CMD_HD_RDDMA_REG, 0x00, NULL, buf + done, c) != 0) {
            at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0);
            s_pkt.in_progress = false;
            error = true;
            goto done;
        }
        done += c;
    }
    s_pkt.read_len += to_read;
    ret = (int)to_read;

    if (s_pkt.read_len >= s_pkt.total_len) {
        if (at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0) != 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0) != 0) {
                /* INT0 两次失败：不更新 s_last_consumed_seq，下轮重试 */
                error = true;
                goto done;
            }
        }
        s_last_consumed_seq = s_pkt.seq;
        s_pkt.in_progress = false;
    }

done:
    xSemaphoreGive(s_bus_mutex);
    return error ? -1 : ret;
}