#include "esp_at_spi.h"
#include "hal_spi.h"
#include "freertos_port.h"
#include <string.h>

#ifndef ESP_AT_SPI_DEBUG
#define ESP_AT_SPI_DEBUG  0
#endif

#if ESP_AT_SPI_DEBUG
#include <stdio.h>
#define AT_LOG(fmt, ...)  printf("[AT_SPI] " fmt "\r\n", ##__VA_ARGS__)
#else
#define AT_LOG(fmt, ...)  do {} while (0)
#endif

#define AT_WAIT_IRQ_MAX_MS  200

static SemaphoreHandle_t s_bus_mutex = NULL;
static uint8_t           s_send_seq  = 0;
static uint8_t           s_last_consumed_seq = 0;
static bool              s_seq_valid = false;    /* S2: 首包不被误判 */
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
    s_last_consumed_seq = 0;
    s_seq_valid = false;
    s_pkt.in_progress = false;
    s_inited = 1;
    return 0;
}

int esp_at_spi_reset_and_wait_ready(uint32_t timeout_ms)
{
    hal_c5_reset();
    s_pkt.in_progress = false;
    s_send_seq = 0;
    s_last_consumed_seq = 0;
    s_seq_valid = false;
    return hal_spi_wait_irq(timeout_ms);
}

int esp_at_spi_wait_irq(uint32_t timeout_ms)
{
    return hal_spi_wait_irq(timeout_ms);
}

/* ============================================================
 * 写
 *   S1: WRDMA 成功后立刻累加 done；WR_END 失败只记错不重发
 *   S3: 内部等待超时按剩余时间裁剪
 *   P6: request 失败不递增 seq
 * ============================================================ */
int esp_at_spi_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!data || !len) return -1;
    if (xSemaphoreTake(s_bus_mutex, portMAX_DELAY) != pdTRUE) return -1;

    size_t done = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t to    = pdMS_TO_TICKS(timeout_ms);

    while (done < len) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= to) break;

        size_t chunk = len - done;
        if (chunk > SPI_AT_MAX_PAYLOAD) chunk = SPI_AT_MAX_PAYLOAD;

        uint8_t next_seq = (s_send_seq + 1) & 0xFF;
        if (at_request_write(next_seq, (uint16_t)chunk) != 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;   /* seq 未变，下次重试同一 seq */
        }
        s_send_seq = next_seq;

        /* S3: 剩余时间裁剪 */
        TickType_t remain = to - (xTaskGetTickCount() - start);
        uint32_t remain_ms = (uint32_t)(remain * portTICK_PERIOD_MS);
        if (remain_ms > AT_WAIT_IRQ_MAX_MS) remain_ms = AT_WAIT_IRQ_MAX_MS;

        if (hal_spi_wait_irq(remain_ms) != 0) break;

        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) break;
        if (opt.direct != SPI_DIR_WRITE) break;

        size_t send_len = (chunk < opt.transmit_len) ? chunk : opt.transmit_len;
        if (send_len == 0 || send_len > SPI_AT_MAX_PAYLOAD) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (at_spi_xfer(CMD_HD_WRDMA_REG, 0x00, data + done, NULL, send_len) != 0) {
            break;
        }
        /* S1: WRDMA 已成功，无论 WR_END 结果如何都不重发这段 */
        done += send_len;

        if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
                break;   /* WR_END 两次失败，放弃本轮；seq 与数据已推进 */
            }
        }
    }

    xSemaphoreGive(s_bus_mutex);
    return (int)done;
}

/* ============================================================
 * 读
 *   S2: 用 s_seq_valid 标志，首包不被误判
 * ============================================================ */
int esp_at_spi_read(uint8_t *buf, size_t buf_size)
{
    if (!buf || !buf_size) return 0;
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;

    int ret = 0;
    bool error = false;

    if (!s_pkt.in_progress) {
        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) goto done;
        if (opt.direct != SPI_DIR_READ || opt.transmit_len == 0) goto done;

        /* S2: 只有已经消费过至少一包，seq 去重才有意义 */
        if (s_seq_valid && opt.seq_num == s_last_consumed_seq) goto done;

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
                error = true;
                goto done;
            }
        }
        s_last_consumed_seq = s_pkt.seq;
        s_seq_valid = true;
        s_pkt.in_progress = false;
    }

done:
    xSemaphoreGive(s_bus_mutex);
    return error ? -1 : ret;
}