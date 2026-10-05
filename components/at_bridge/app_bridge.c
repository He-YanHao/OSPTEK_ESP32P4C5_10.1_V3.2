#include "app_bridge.h"
#include "esp_at_spi.h"
#include "hal_uart.h"
#include "freertos_port.h"
#include <string.h>

/*
 * 单例说明：
 *   本模块所有任务/缓冲区均为单例，进程内只允许一次 app_bridge_start。
 */

#ifndef APP_BRIDGE_DEBUG
#define APP_BRIDGE_DEBUG  0
#endif

#if APP_BRIDGE_DEBUG
#include <stdio.h>
#define BR_LOG(fmt, ...)  printf("[BRIDGE] " fmt "\r\n", ##__VA_ARGS__)
#else
#define BR_LOG(fmt, ...)  do {} while (0)
#endif

#define SPI_TASK_STACK   6144
#define UTX_TASK_STACK   6144   /* 栈上有 2KB 缓冲，需更大栈 */
#define SPI_TASK_PRIO    3
#define UTX_TASK_PRIO    3

#define U2C_WRITE_TIMEOUT_MS  100   /* 单次写 C5 的短超时 */

static StreamBufferHandle_t s_s2u_sb = NULL;
static TaskHandle_t         s_spi_task_h = NULL;
static TaskHandle_t         s_utx_task_h = NULL;

/* ============================================================
 * UART TX 任务
 *   F3: 用栈缓冲，避免 vTaskDelete 时的内存泄漏
 * ============================================================ */
static void uart_tx_task(void *arg)
{
    (void)arg;

    uint8_t buf[APP_BRIDGE_UART_CHUNK];
    uint8_t pending[APP_BRIDGE_UART_CHUNK];
    size_t  pend_len = 0;

    while (1) {
        /* 优先处理 pending 残留 */
        if (pend_len > 0) {
            int w = hal_uart_write(pending, pend_len);
            if (w > 0) {
                if ((size_t)w >= pend_len) {
                    pend_len = 0;
                } else {
                    memmove(pending, pending + w, pend_len - w);
                    pend_len -= w;
                }
            }
            if (pend_len > 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;   /* 未清空前不接收新数据 */
            }
        }

        size_t n = xStreamBufferReceive(s_s2u_sb, buf,
                                        APP_BRIDGE_UART_CHUNK,
                                        pdMS_TO_TICKS(20));
        if (n > 0) {
            int w = hal_uart_write(buf, n);
            if (w < 0) w = 0;
            if ((size_t)w < n) {
                size_t rest = n - (size_t)w;
                /* I4: pending 与 buf 同大，rest ≤ n ≤ UART_CHUNK = sizeof(pending) */
                memcpy(pending, buf + w, rest);
                pend_len = rest;
            }
        }
    }
}

/* ============================================================
 * SPI 任务
 *   S2: 读优先，写分片短超时
 *   F1: pend 用 append 而非覆盖
 * ============================================================ */
static void spi_task(void *arg)
{
    (void)arg;

    uint8_t *rx = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
    uint8_t *tx = pvPortMalloc(APP_BRIDGE_UART_CHUNK);
    if (!rx || !tx) {
        BR_LOG("malloc failed");
        vPortFree(rx);
        vPortFree(tx);
        vTaskDelete(NULL);
        return;
    }

    /* pend 容量 = 2 × UART_CHUNK，append 追加 */
    static uint8_t pend[APP_BRIDGE_UART_CHUNK * 2];
    static size_t  pend_len = 0;

    BR_LOG("spi_task started");

    while (1) {
        /* ============================================================
         * 方向 1：C5 → 消费者（优先）
         *   有 IRQ 且 SB 有空间就读，读完继续回顶看新 IRQ
         * ============================================================ */
        if (xStreamBufferSpacesAvailable(s_s2u_sb) >= APP_BRIDGE_SPI_CHUNK) {
            if (esp_at_spi_wait_irq(0) == 0) {
                for (int i = 0; i < 32; i++) {
                    if (xStreamBufferSpacesAvailable(s_s2u_sb) <
                        APP_BRIDGE_SPI_CHUNK) {
                        break;
                    }
                    int r = esp_at_spi_read(rx, APP_BRIDGE_SPI_CHUNK);
                    if (r == 0) break;
                    if (r < 0) break;
                    size_t sent = xStreamBufferSend(s_s2u_sb, rx, r,
                                                    pdMS_TO_TICKS(50));
                    if (sent != (size_t)r) {
                        BR_LOG("s2u ovf %u/%d", (unsigned)sent, r);
                        break;
                    }
                }
                continue;   /* 读完立即回顶，看是否还有新 IRQ */
            }
        }

        /* ============================================================
         * 方向 2：UART → C5（分片短超时，不长时间占用 SPI）
         * ============================================================ */

        /* 先处理 pend 残留：短超时写一小段 */
        if (pend_len > 0) {
            int w = esp_at_spi_write(pend, pend_len, U2C_WRITE_TIMEOUT_MS);
            if (w > 0) {
                if ((size_t)w >= pend_len) {
                    pend_len = 0;
                } else {
                    memmove(pend, pend + w, pend_len - w);
                    pend_len -= w;
                }
            }
            if (pend_len > 0) {
                /* 未清空：本轮不再读新 UART，让出后回顶看 IRQ */
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        /* pend 空：读新 UART 数据 */
        int n = hal_uart_read(tx, APP_BRIDGE_UART_CHUNK, 0);
        if (n > 0) {
            int w = esp_at_spi_write(tx, n, U2C_WRITE_TIMEOUT_MS);
            if (w < 0) w = 0;
            if ((size_t)w < (size_t)n) {
                size_t rest = (size_t)n - (size_t)w;
                /* append 而非覆盖 */
                if (rest <= sizeof(pend) - pend_len) {
                    memcpy(pend + pend_len, tx + w, rest);
                    pend_len += rest;
                } else {
                    BR_LOG("pend ovf: need %u, free %u",
                           (unsigned)rest,
                           (unsigned)(sizeof(pend) - pend_len));
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* ============================================================
 * 启动
 *   S1: 初始 flush 走 StreamBuffer，不直接 hal_uart_write
 *   F3: 栈缓冲
 * ============================================================ */
int app_bridge_start(void)
{
    if (s_s2u_sb != NULL) return 0;

    if (hal_uart_init() != 0) {
        BR_LOG("hal_uart_init failed");
        return -1;
    }
    if (esp_at_spi_init() != 0) {
        BR_LOG("esp_at_spi_init failed");
        return -1;
    }

    s_s2u_sb = xStreamBufferCreate(APP_BRIDGE_SB_SIZE, 1);
    if (!s_s2u_sb) return -1;

    /* S1: 先创建 uart_tx_task 再 flush，flush 数据走 SB */
    if (xTaskCreate(uart_tx_task, "br_utx", UTX_TASK_STACK,
                    NULL, UTX_TASK_PRIO, &s_utx_task_h) != pdPASS) {
        BR_LOG("utx task create failed");
        vStreamBufferDelete(s_s2u_sb);
        s_s2u_sb = NULL;
        return -1;
    }

    /* 复位 C5 并 flush ready 消息 */
    if (esp_at_spi_reset_and_wait_ready(5000) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t *buf = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
        if (buf) {
            for (int i = 0; i < 8; i++) {
                int r = esp_at_spi_read(buf, APP_BRIDGE_SPI_CHUNK);
                if (r <= 0) break;
                size_t sent = xStreamBufferSend(s_s2u_sb, buf, r,
                                                pdMS_TO_TICKS(100));
                if (sent != (size_t)r) break;
            }
            vPortFree(buf);
        }
    } else {
        BR_LOG("no ready pulse");
    }

    if (xTaskCreate(spi_task, "br_spi", SPI_TASK_STACK,
                    NULL, SPI_TASK_PRIO, &s_spi_task_h) != pdPASS) {
        BR_LOG("spi task create failed");
        vTaskDelete(s_utx_task_h);
        s_utx_task_h = NULL;
        vStreamBufferDelete(s_s2u_sb);
        s_s2u_sb = NULL;
        return -1;
    }
    return 0;
}