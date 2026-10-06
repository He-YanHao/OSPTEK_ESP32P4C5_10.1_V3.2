#include "app_bridge.h"
#include "esp_at_spi.h"
#include "hal_uart.h"
#include "freertos_port.h"
#include <string.h>

/* ============================================================
 *  调试日志开关
 * ============================================================ */
#ifndef APP_BRIDGE_DEBUG
#define APP_BRIDGE_DEBUG  0
#endif

#if APP_BRIDGE_DEBUG
#include <stdio.h>
#define BR_LOG(fmt, ...)  printf("[BRIDGE] " fmt "\r\n", ##__VA_ARGS__)
#else
#define BR_LOG(fmt, ...)  do {} while (0)
#endif

/* ---------- 任务配置 ---------- */
#define SPI_TASK_STACK   6144       /* SPI 任务栈 */
#define UTX_TASK_STACK   6144       /* UART 发送任务栈（栈上有 2KB 缓冲） */
#define SPI_TASK_PRIO    3
#define UTX_TASK_PRIO    3          /* 与 SPI 任务同级，时间片轮转 */

/* 单次写 C5 的短超时：避免长时间占用 SPI 总线 */
#define U2C_WRITE_TIMEOUT_MS  100

/* ---------- 全局句柄 ---------- */
static StreamBufferHandle_t s_s2u_sb = NULL;     /* SPI → UART 的缓冲 */
static TaskHandle_t         s_spi_task_h = NULL;
static TaskHandle_t         s_utx_task_h = NULL;

/* ============================================================
 * UART 发送任务（C5 → PC 方向）
 * ============================================================
 *
 * 【职责】
 *   从 StreamBuffer 取数据，通过 hal_uart_write 写入 UART。
 *
 * 【关键设计】
 *   - 用栈缓冲，避免 pvPortMalloc 在 vTaskDelete 时泄漏
 *   - 有本地 pending 缓冲承接 hal_uart_write 的部分写
 *   - pending 未清空前不接收新数据，保证字节顺序
 */
static void uart_tx_task(void *arg)
{
    (void)arg;

    /* 栈上 2KB 缓冲：buf 用于接收新数据，pending 用于保留未写完部分 */
    uint8_t buf[APP_BRIDGE_UART_CHUNK];
    uint8_t pending[APP_BRIDGE_UART_CHUNK];
    size_t  pend_len = 0;

    while (1) {
        /* ============================================================
         * 优先处理 pending 残留（上次未写完的数据）
         * ============================================================ */
        if (pend_len > 0) {
            int w = hal_uart_write(pending, pend_len);
            if (w > 0) {
                if ((size_t)w >= pend_len) {
                    /* 全部写完，清空 pending */
                    pend_len = 0;
                } else {
                    /* 部分写完，保留剩余 */
                    memmove(pending, pending + w, pend_len - w);
                    pend_len -= w;
                }
            }
            if (pend_len > 0) {
                /* pending 未清空：不接收新数据，保证顺序 */
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        /* ============================================================
         * 从 StreamBuffer 取新数据（20ms 超时，避免空转）
         * ============================================================ */
        size_t n = xStreamBufferReceive(s_s2u_sb, buf,
                                        APP_BRIDGE_UART_CHUNK,
                                        pdMS_TO_TICKS(20));
        if (n > 0) {
            int w = hal_uart_write(buf, n);
            if (w < 0) w = 0;
            if ((size_t)w < n) {
                /* 部分写入：剩余存到 pending */
                size_t rest = n - (size_t)w;
                /* pending 与 buf 同大，rest ≤ n ≤ UART_CHUNK = sizeof(pending) */
                memcpy(pending, buf + w, rest);
                pend_len = rest;
            }
        }
    }
}

/* ============================================================
 * SPI 主任务（双向搬运）
 * ============================================================
 *
 * 【职责】
 *   单任务顺序处理两个方向，避免 IRQ 争抢和状态机并发问题。
 *
 * 【优先级顺序】
 *   1. 优先读路径（C5 → PC）：有 IRQ 且 SB 有空间就读
 *   2. 其次写路径（PC → C5）：处理 pending / 读 UART / 写 C5
 *
 * 【流控】
 *   读路径只在 SB 有空间时进行，SB 满 → 停止读 C5 → TCP 背压。
 */
static void spi_task(void *arg)
{
    (void)arg;

    /* 分配两个工作缓冲 */
    uint8_t *rx = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
    uint8_t *tx = pvPortMalloc(APP_BRIDGE_UART_CHUNK);
    if (!rx || !tx) {
        BR_LOG("malloc failed");
        vPortFree(rx);
        vPortFree(tx);
        vTaskDelete(NULL);
        return;
    }

    /* pend：写路径未送达 C5 的残留数据，容量 = 2 × UART_CHUNK */
    static uint8_t pend[APP_BRIDGE_UART_CHUNK * 2];
    static size_t  pend_len = 0;

    BR_LOG("spi_task started");

    while (1) {
        /* ============================================================
         * 方向 1：C5 → 消费者（优先）
         *   条件：StreamBuffer 有足够空间
         *   动作：等 IRQ，读到无数据或 SB 满
         * ============================================================ */
        if (xStreamBufferSpacesAvailable(s_s2u_sb) >= APP_BRIDGE_SPI_CHUNK) {
            if (esp_at_spi_wait_irq(0) == 0) {
                /* 单次 IRQ 内循环读，最多 32 次防止死循环 */
                for (int i = 0; i < 32; i++) {
                    /* 每次读前再检查一次空间 */
                    if (xStreamBufferSpacesAvailable(s_s2u_sb) <
                        APP_BRIDGE_SPI_CHUNK) {
                        break;
                    }
                    int r = esp_at_spi_read(rx, APP_BRIDGE_SPI_CHUNK);
                    if (r == 0) break;   /* 无数据 */
                    if (r < 0) break;    /* 错误 */
                    size_t sent = xStreamBufferSend(s_s2u_sb, rx, r,
                                                    pdMS_TO_TICKS(50));
                    if (sent != (size_t)r) {
                        BR_LOG("s2u ovf %u/%d", (unsigned)sent, r);
                        break;
                    }
                }
                continue;   /* 读完立即回顶，看是否有新 IRQ */
            }
        }

        /* ============================================================
         * 方向 2：UART → C5（分片短超时，不长时间占用 SPI）
         * ============================================================ */

        /* -------- 先处理 pend 残留 -------- */
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
                /* 未清空：本轮不再读新 UART */
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        /* -------- pend 空：读新 UART 数据 -------- */
        int n = hal_uart_read(tx, APP_BRIDGE_UART_CHUNK, 0);
        if (n > 0) {
            int w = esp_at_spi_write(tx, n, U2C_WRITE_TIMEOUT_MS);
            if (w < 0) w = 0;
            if ((size_t)w < (size_t)n) {
                size_t rest = (size_t)n - (size_t)w;
                /* append 而非覆盖，保证顺序 */
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
            /* 无新数据：让出 1ms，避免空转 */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* ============================================================
 * 启动桥接
 * ============================================================
 *
 * 【初始化顺序】
 *   1. hal_uart_init()        先初始化消费者
 *   2. esp_at_spi_init()      再初始化 SPI 协议层
 *   3. 创建 StreamBuffer
 *   4. 先创建 uart_tx_task    （flush ready 消息需要它消费 SB）
 *   5. 复位 C5 并 flush ready 消息（走 SB，由 uart_tx_task 转发）
 *   6. 最后创建 spi_task
 *
 * 【失败回滚】
 *   任何一步失败都回滚已创建的资源。
 */
int app_bridge_start(void)
{
    /* 幂等：已启动过直接返回 */
    if (s_s2u_sb != NULL) return 0;

    /* -------- 步骤 1/2：初始化 HAL -------- */
    if (hal_uart_init() != 0) {
        BR_LOG("hal_uart_init failed");
        return -1;
    }
    if (esp_at_spi_init() != 0) {
        BR_LOG("esp_at_spi_init failed");
        return -1;
    }

    /* -------- 步骤 3：创建 StreamBuffer -------- */
    s_s2u_sb = xStreamBufferCreate(APP_BRIDGE_SB_SIZE, 1);
    if (!s_s2u_sb) return -1;

    /* -------- 步骤 4：先创建 uart_tx_task（flush 需要它） -------- */
    if (xTaskCreate(uart_tx_task, "br_utx", UTX_TASK_STACK,
                    NULL, UTX_TASK_PRIO, &s_utx_task_h) != pdPASS) {
        BR_LOG("utx task create failed");
        vStreamBufferDelete(s_s2u_sb);
        s_s2u_sb = NULL;
        return -1;
    }

    /* -------- 步骤 5：复位 C5 并 flush ready 消息 -------- */
    if (esp_at_spi_reset_and_wait_ready(5000) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t *buf = pvPortMalloc(APP_BRIDGE_SPI_CHUNK);
        if (buf) {
            /* 最多读 8 次，把 C5 上电后的 ready 消息读干净 */
            for (int i = 0; i < 8; i++) {
                int r = esp_at_spi_read(buf, APP_BRIDGE_SPI_CHUNK);
                if (r <= 0) break;
                /* 通过 SB 转发，由 uart_tx_task 消费 */
                size_t sent = xStreamBufferSend(s_s2u_sb, buf, r,
                                                pdMS_TO_TICKS(100));
                if (sent != (size_t)r) break;
            }
            vPortFree(buf);
        }
    } else {
        BR_LOG("no ready pulse");
    }

    /* -------- 步骤 6：创建 spi_task -------- */
    if (xTaskCreate(spi_task, "br_spi", SPI_TASK_STACK,
                    NULL, SPI_TASK_PRIO, &s_spi_task_h) != pdPASS) {
        BR_LOG("spi task create failed");
        /* 回滚：删 uart_tx_task + SB */
        vTaskDelete(s_utx_task_h);
        s_utx_task_h = NULL;
        vStreamBufferDelete(s_s2u_sb);
        s_s2u_sb = NULL;
        return -1;
    }
    return 0;
}