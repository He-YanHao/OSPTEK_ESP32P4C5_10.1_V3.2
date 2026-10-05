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

#define AT_WAIT_IRQ_MAX_MS   200
#define AT_RETRY_BACKOFF_MS  1

static SemaphoreHandle_t s_bus_mutex = NULL;
static uint8_t           s_send_seq  = 0;
static uint8_t           s_last_consumed_seq = 0;
static bool              s_seq_valid = false;
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
 *   F1: IRQ 超时不放弃，剩余预算内继续重试
 *   F2: WR_END 双失败时 done -= send_len，返回部分长度
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
        TickType_t remain = to - elapsed;

        size_t chunk = len - done;
        if (chunk > SPI_AT_MAX_PAYLOAD) chunk = SPI_AT_MAX_PAYLOAD;

        uint8_t next_seq = (s_send_seq + 1) & 0xFF;
        if (at_request_write(next_seq, (uint16_t)chunk) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }
        s_send_seq = next_seq;

        uint32_t remain_ms = (uint32_t)(remain * portTICK_PERIOD_MS);
        if (remain_ms > AT_WAIT_IRQ_MAX_MS) remain_ms = AT_WAIT_IRQ_MAX_MS;

        if (hal_spi_wait_irq(remain_ms) != 0) {
            /* F1: IRQ 超时不放弃，回到循环顶部检查剩余预算 */
            AT_LOG("wait_irq timeout, retry");
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }

        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }
        if (opt.direct != SPI_DIR_WRITE) {
            /* C5 有 READ 数据要发，让路 */
            AT_LOG("slave not writable: direct=%u", opt.direct);
            break;
        }

        size_t send_len = (chunk < opt.transmit_len) ? chunk : opt.transmit_len;
        if (send_len == 0 || send_len > SPI_AT_MAX_PAYLOAD) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }

        if (at_spi_xfer(CMD_HD_WRDMA_REG, 0x00, data + done, NULL, send_len) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;   /* WRDMA 失败，重试同一段 */
        }

        /* F2: WRDMA 成功但 WR_END 失败 = 这段未提交，退回 done */
        if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
                AT_LOG("WR_END failed twice, rollback %u bytes",
                       (unsigned)send_len);
                break;   /* done 未累加，上层会重试这段 */
            }
        }

        done += send_len;
    }

    xSemaphoreGive(s_bus_mutex);
    return (int)done;
}

/* ============================================================
 * 读
 *   S3: INT0 失败时不更新 s_last_consumed_seq
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
        bool int0_ok = false;
        if (at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0) == 0) {
            int0_ok = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0) == 0) {
                int0_ok = true;
            }
        }

        if (int0_ok) {
            s_last_consumed_seq = s_pkt.seq;
            s_seq_valid = true;
        } else {
            /* S3: INT0 失败，不更新 last_consumed。
             * 清 in_progress 避免卡死。
             * 下次 read 会看到同一 seq，因为 s_last_consumed 未更新，
             * 会重新进入 in_progress 分支尝试重读——但 C5 侧此时可能
             * 已推进。为兼容此情况，同时重置 s_seq_valid 让去重失效一次。 */
            AT_LOG("INT0 failed, will resync on next read");
            s_seq_valid = false;
            error = true;
        }
        s_pkt.in_progress = false;
    }

done:
    xSemaphoreGive(s_bus_mutex);
    return error ? -1 : ret;
}