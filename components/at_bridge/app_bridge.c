#include "app_bridge.h"
#include "esp_at_spi.h"
#include "hal_uart.h"
#include "freertos_port.h"
#include <string.h>

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
#define UTX_TASK_STACK   4096
#define SPI_TASK_PRIO    3
#define UTX_TASK_PRIO    3   /* 与 spi_task 同级，时间片轮转 */

static StreamBufferHandle_t s_s2u_sb = NULL;

/* ============================================================
 * UART TX 任务：从 StreamBuffer 取数据 → hal_uart_write
 * ============================================================ */
static void uart_tx_task(void *arg)
{
    (void)arg;
    uint8_t *buf = pvPortMalloc(APP_BRIDGE_UART_CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }

    while (1) {
        size_t n = xStreamBufferReceive(s_s2u_sb, buf,
                                        APP_BRIDGE_UART_CHUNK,
                                        portMAX_DELAY);
        if (n > 0) {
            /* 契约：hal_uart_write 阻塞直到写完或超时。
             * 返回值 < n 表示超时，剩余丢弃（PC 已断开）。 */
            int w = hal_uart_write(buf, n);
            if (w < (int)n) {
                BR_LOG("uart_write %d/%u (dropped)", w, (unsigned)n);
            }
        }
    }
}

/* ============================================================
 * 主 SPI 任务：单任务顺序处理两个方向
 *   F2 修复：不再用 continue 短路写路径
 *   F3 契约：hal_uart_write 不再丢弃；这里只负责流控
 * ============================================================ */
static void spi_task(void *arg)
{
    (void)arg;

    uint8_t *rx = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
    uint8_t *tx = pvPortMalloc(APP_BRIDGE_UART_CHUNK);
    if (!rx || !tx) {
        BR_LOG("malloc failed");
        vTaskDelete(NULL);
        return;
    }

    static uint8_t pend[APP_BRIDGE_UART_CHUNK];
    static size_t  pend_len = 0;

    BR_LOG("spi_task started");

    while (1) {
        /* ============================================================
         * 方向 2：PC → C5（每轮都处理，优先实时性）
         * ============================================================ */
        if (pend_len > 0) {
            int w = esp_at_spi_write(pend, pend_len, 1000);
            if (w > 0) {
                if ((size_t)w >= pend_len) {
                    pend_len = 0;
                } else {
                    memmove(pend, pend + w, pend_len - w);
                    pend_len -= w;
                }
            }
        }

        int n = hal_uart_read(tx, APP_BRIDGE_UART_CHUNK, 0);  /* 非阻塞 */
        if (n > 0) {
            int w = esp_at_spi_write(tx, n, 1000);
            if (w < 0) w = 0;
            if (w < n) {
                size_t rest = (size_t)(n - w);
                if (rest <= sizeof(pend)) {
                    memcpy(pend, tx + w, rest);
                    pend_len = rest;
                } else {
                    BR_LOG("pend ovf, drop %u", (unsigned)rest);
                }
            }
        }

        /* ============================================================
         * 方向 1：C5 → 消费者（有空间才读，触发 TCP 背压）
         * ============================================================ */
        size_t sb_free = xStreamBufferSpacesAvailable(s_s2u_sb);

        if (sb_free < APP_BRIDGE_SPI_CHUNK) {
            /* 下游满：等 UART 排空 */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* 有空间：看是否有 IRQ */
        if (esp_at_spi_wait_irq(0) == 0) {
            for (int i = 0; i < 4; i++) {
                if (xStreamBufferSpacesAvailable(s_s2u_sb) < APP_BRIDGE_SPI_CHUNK) {
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
        } else {
            /* 无 IRQ：让出 1ms */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* ============================================================
 * 启动
 *   F1 修复：显式调 hal_uart_init()
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
    if (!s_s2u_sb) {
        BR_LOG("sb create failed");
        return -1;
    }

    if (esp_at_spi_reset_and_wait_ready(5000) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t *buf = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
        if (buf) {
            for (int i = 0; i < 8; i++) {
                int r = esp_at_spi_read(buf, APP_BRIDGE_SPI_CHUNK);
                if (r <= 0) break;
                hal_uart_write(buf, r);
            }
            vPortFree(buf);
        }
    } else {
        BR_LOG("no ready pulse");
    }

    BaseType_t r1 = xTaskCreate(spi_task, "br_spi", SPI_TASK_STACK,
                                NULL, SPI_TASK_PRIO, NULL);
    BaseType_t r2 = xTaskCreate(uart_tx_task, "br_utx", UTX_TASK_STACK,
                                NULL, UTX_TASK_PRIO, NULL);
    if (r1 != pdPASS || r2 != pdPASS) {
        BR_LOG("task create failed");
        return -1;
    }
    return 0;
}