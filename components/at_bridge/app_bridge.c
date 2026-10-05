#include "app_bridge.h"
#include "esp_at_spi.h"
#include "hal_uart.h"
#include "freertos_port.h"
#include <string.h>
#include <stdio.h>

#define BR_LOG(fmt, ...)  printf("[BRIDGE] " fmt "\r\n", ##__VA_ARGS__)

#define SPI_TASK_STACK   6144
#define UTX_TASK_STACK   4096
#define SPI_TASK_PRIO    5
#define UTX_TASK_PRIO    4

static StreamBufferHandle_t s_s2u_sb = NULL;

static void uart_tx_task(void *arg)
{
    (void)arg;
    uint8_t *buf = pvPortMalloc(APP_BRIDGE_UART_CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }

    BR_LOG("uart_tx started");

    while (1) {
        size_t n = xStreamBufferReceive(s_s2u_sb, buf,
                                        APP_BRIDGE_UART_CHUNK,
                                        portMAX_DELAY);
        if (n > 0) {
            int w = hal_uart_write(buf, n);
            if (w != (int)n) BR_LOG("uart_write %d/%u", w, (unsigned)n);
        }
    }
}

/* ============================================================
 * SPI 任务：单任务顺序处理读和写，避免 IRQ 争抢
 *
 * ★ 修复：方向 1 的 wait_irq 判返回值。有 IRQ 才读；没 IRQ 直接
 *   进写路径。这样写路径的 IRQ 不会被读路径抢走。
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
        /* ---------- 方向 1：C5 → UART ----------
         * ★ 有 IRQ 才尝试读。读不到就落写路径。 */
        if (esp_at_spi_wait_irq(20) == 0) {
            for (int i = 0; i < 16; i++) {
                int r = esp_at_spi_read(rx, APP_BRIDGE_SPI_CHUNK);
                if (r == 0) break;
                if (r < 0) {
                    BR_LOG("spi_read err");
                    break;
                }
                size_t sent = xStreamBufferSend(s_s2u_sb, rx, r,
                                                pdMS_TO_TICKS(200));
                if (sent != (size_t)r) {
                    BR_LOG("s2u ovf %u/%d", (unsigned)sent, r);
                }
            }
            continue;   /* 处理完读路径，回顶部 */
        }

        /* ---------- 方向 2：UART → C5 ---------- */
        if (pend_len > 0) {
            int w = esp_at_spi_write(pend, pend_len, 2000);
            if (w > 0) {
                if ((size_t)w >= pend_len) {
                    pend_len = 0;
                } else {
                    memmove(pend, pend + w, pend_len - w);
                    pend_len -= w;
                }
            }
            if (pend_len > 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        int n = hal_uart_read(tx, APP_BRIDGE_UART_CHUNK, 20);
        if (n > 0) {
            BR_LOG("uart_read %d bytes", n);
            int w = esp_at_spi_write(tx, n, 2000);
            BR_LOG("spi_write ret=%d/%d", w, n);
            if (w < 0) w = 0;
            if (w < n) {
                size_t rest = (size_t)(n - w);
                if (rest <= sizeof(pend)) {
                    memcpy(pend, tx + w, rest);
                    pend_len = rest;
                } else {
                    BR_LOG("pend ovf %u", (unsigned)rest);
                }
            }
        }
    }
}

int app_bridge_start(void)
{
    if (s_s2u_sb != NULL) return 0;

    if (esp_at_spi_init() != 0) {
        BR_LOG("esp_at_spi_init failed");
        return -1;
    }

    s_s2u_sb = xStreamBufferCreate(APP_BRIDGE_SB_SIZE, 1);
    if (!s_s2u_sb) return -1;

    if (esp_at_spi_reset_and_wait_ready(5000) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t *buf = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
        if (buf) {
            for (int i = 0; i < 8; i++) {
                int r = esp_at_spi_read(buf, APP_BRIDGE_SPI_CHUNK);
                if (r <= 0) break;
                BR_LOG("ready read %d bytes", r);
                hal_uart_write(buf, r);
            }
            vPortFree(buf);
        }
    } else {
        BR_LOG("no ready pulse");
    }

    xTaskCreate(spi_task, "br_spi", SPI_TASK_STACK, NULL, SPI_TASK_PRIO, NULL);
    xTaskCreate(uart_tx_task, "br_utx", UTX_TASK_STACK, NULL, UTX_TASK_PRIO, NULL);
    return 0;
}