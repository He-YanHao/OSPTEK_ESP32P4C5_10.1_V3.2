#include "esp_at_spi.h"
#include "hal_spi.h"
#include "freertos_port.h"
#include <string.h>
#include <stdio.h>

/* ★ 调试日志默认打开，跑通后注释掉下一行 */
#define ESP_AT_SPI_DEBUG

#ifdef ESP_AT_SPI_DEBUG
#define AT_LOG(fmt, ...)  printf("[AT_SPI] " fmt "\r\n", ##__VA_ARGS__)
#else
#define AT_LOG(fmt, ...)  do {} while (0)
#endif

static SemaphoreHandle_t s_bus_mutex = NULL;
static uint8_t           s_send_seq  = 0;
static uint8_t           s_last_consumed_seq = 0xFF;
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
    AT_LOG("init ok");
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
 * 写：WRBUF → 等 IRQ → RDBUF 确认 → WRDMA(send_len) → WR_END
 *
 * ★ 修复核心：WRBUF 后必须等 IRQ，再查 RDBUF。
 *   C5 的 RDBUF 更新滞后于 IRQ，如果只轮询 RDBUF，会读到旧状态。
 * ============================================================ */
int esp_at_spi_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!data || !len) return -1;
    if (xSemaphoreTake(s_bus_mutex, portMAX_DELAY) != pdTRUE) return -1;

    size_t done = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t to    = pdMS_TO_TICKS(timeout_ms);

    while (done < len) {
        if ((xTaskGetTickCount() - start) >= to) {
            AT_LOG("write timeout %u/%u", (unsigned)done, (unsigned)len);
            break;
        }

        size_t chunk = len - done;
        if (chunk > SPI_AT_MAX_PAYLOAD) chunk = SPI_AT_MAX_PAYLOAD;

        s_send_seq = (s_send_seq + 1) & 0xFF;
        if (at_request_write(s_send_seq, (uint16_t)chunk) != 0) {
            AT_LOG("WRBUF fail seq=%u", s_send_seq);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* ★ 关键修复：等 IRQ。C5 收到 WRBUF 后会拉 IRQ 通知状态已更新 */
        if (hal_spi_wait_irq(200) != 0) {
            AT_LOG("no IRQ after WRBUF seq=%u", s_send_seq);
            break;
        }

        /* 再查 RDBUF，此时状态是最新的 */
        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) {
            AT_LOG("query fail after IRQ");
            break;
        }

        AT_LOG("WRBUF resp: direct=%u seq=%u len=%u",
               opt.direct, opt.seq_num, opt.transmit_len);

        if (opt.direct != SPI_DIR_WRITE) {
            AT_LOG("direct=%u not WRITE", opt.direct);
            break;
        }

        size_t send_len = (chunk < opt.transmit_len) ? chunk : opt.transmit_len;
        if (send_len == 0 || send_len > SPI_AT_MAX_PAYLOAD) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (at_spi_xfer(CMD_HD_WRDMA_REG, 0x00, data + done, NULL, send_len) != 0) {
            AT_LOG("WRDMA fail");
            break;
        }
        if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
            AT_LOG("WR_END fail");
            break;
        }

        AT_LOG("sent %u bytes (seq=%u)", (unsigned)send_len, s_send_seq);
        done += send_len;
    }

    xSemaphoreGive(s_bus_mutex);
    return (int)done;
}

/* ============================================================
 * 读：RDBUF → RDDMA → INT0
 *   seq 去重防止同一包反复读
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
            AT_LOG("read: query fail");
            goto done;
        }
        if (opt.direct != SPI_DIR_READ || opt.transmit_len == 0) {
            goto done;
        }

        if (opt.seq_num == s_last_consumed_seq) {
            AT_LOG("read: dup seq=%u, skip", opt.seq_num);
            goto done;
        }

        s_pkt.seq         = opt.seq_num;
        s_pkt.total_len   = opt.transmit_len;
        s_pkt.read_len    = 0;
        s_pkt.in_progress = true;
        AT_LOG("read: new pkt seq=%u len=%u", s_pkt.seq, s_pkt.total_len);
    }

    size_t remain  = s_pkt.total_len - s_pkt.read_len;
    size_t to_read = (remain < buf_size) ? remain : buf_size;

    size_t done = 0;
    while (done < to_read) {
        size_t c = to_read - done;
        if (c > SPI_AT_MAX_PAYLOAD) c = SPI_AT_MAX_PAYLOAD;
        if (at_spi_xfer(CMD_HD_RDDMA_REG, 0x00, NULL, buf + done, c) != 0) {
            AT_LOG("RDDMA fail");
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
                AT_LOG("INT0 fail");
                error = true;
                goto done;
            }
        }
        s_last_consumed_seq = s_pkt.seq;
        s_pkt.in_progress = false;
        AT_LOG("read: pkt seq=%u done, %d bytes", s_last_consumed_seq, ret);
    }

done:
    xSemaphoreGive(s_bus_mutex);
    return error ? -1 : ret;
}