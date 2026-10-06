#include "hal_uart.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ============================================================
 *  P4 平台 UART 配置
 * ============================================================
 *
 * TX/RX 引脚：连到 USB-TTL → PC 串口工具
 * 波特率：    460800（CH340 官方上限，稳定）
 * TX 缓冲：   32KB，给下游 PC 侧留出足够响应时间
 * RX 缓冲：   8KB，PC 端发的 AT 命令都很短
 */
#define P4_UART_PORT      UART_NUM_1
#define P4_UART_TX_PIN    53
#define P4_UART_RX_PIN    51
#define P4_UART_BAUD      460800
#define P4_UART_RX_BUF    (8 * 1024)
#define P4_UART_TX_BUF    (32 * 1024)

/* 单次写入的最大块：越小越频繁唤醒，越大越可能阻塞
 * 256 字节 = 230400 波特率下的 ~11ms 传输时间，是较好的折中 */
#define WRITE_CHUNK       256

/* 无进展超时：500ms 内 TX 一直写不进去就放弃，返回部分写入量
 * 注意：这个超时是"无进展"，不是"总时长"
 * 只要还在持续写入，就不会触发 */
#define WRITE_STALL_MS    500

static const char *TAG = "HAL_UART";
static int s_uart_inited = 0;

/*
 * 初始化 UART 驱动。
 * 幂等：第二次调用直接返回 0。
 * 返回值：0 成功，-1 失败。
 */
int hal_uart_init(void)
{
    if (s_uart_inited) return 0;

    uart_config_t cfg = {
        .baud_rate  = P4_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,   /* 无硬件流控 */
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(P4_UART_PORT, &cfg) != ESP_OK) return -1;
    if (uart_set_pin(P4_UART_PORT,
                     P4_UART_TX_PIN, P4_UART_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -1;
    if (uart_driver_install(P4_UART_PORT,
                            P4_UART_RX_BUF, P4_UART_TX_BUF,
                            0, NULL, 0) != ESP_OK) return -1;

    ESP_LOGI(TAG, "UART%d init: TX=%d RX=%d baud=%d txbuf=%dKB",
             P4_UART_PORT, P4_UART_TX_PIN, P4_UART_RX_PIN,
             P4_UART_BAUD, P4_UART_TX_BUF / 1024);
    s_uart_inited = 1;
    return 0;
}

/*
 * 从 UART 读数据（上行路径：PC → C5）
 * 返回：>0 实际读到的字节数；=0 超时；<0 错误。
 */
int hal_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(P4_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
}

/*
 * 写数据到 UART（下行路径：C5 → PC）。
 *
 * 【契约】
 *   = len  全部写完
 *   < len  部分写完（无进展超时或错误）
 *   < 0    无法写入
 *
 * 【算法】
 *   1. 循环检测 TX ringbuffer 剩余空间
 *   2. 空间不足时 vTaskDelay 让出 CPU（避免看门狗），并计时
 *   3. 只要还有进展（有字节写入），就持续尝试，不触发超时
 *   4. 若 500ms 内一个字节都写不进去，返回已写入量
 *
 * 不主动丢弃数据；上层（app_bridge）负责保存未写完部分重试。
 */
int hal_uart_write(const uint8_t *buf, size_t len)
{
    size_t done = 0;
    TickType_t last_progress = xTaskGetTickCount();
    const TickType_t stall_to = pdMS_TO_TICKS(WRITE_STALL_MS);

    while (done < len) {
        /* 查询 TX ringbuffer 剩余空间 */
        size_t free_space = 0;
        if (uart_get_tx_buffer_free_size(P4_UART_PORT, &free_space) != ESP_OK) {
            /* 查询失败时保守处理：假设只有 32 字节可写 */
            free_space = 32;
        }

        /* 空间不足：等待 ringbuffer 被 UART 硬件排空 */
        if (free_space < 32) {
            if ((xTaskGetTickCount() - last_progress) >= stall_to) {
                /* 500ms 一个字节都写不进去：判定为 stall，返回已写入量 */
                ESP_LOGW(TAG, "uart_write stalled: %u/%u",
                         (unsigned)done, (unsigned)len);
                break;
            }
            /* 让出 CPU，等 UART 硬件排空缓冲 */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* 计算本次写入量：受 WRITE_CHUNK、free_space、剩余数据三者限制 */
        size_t c = len - done;
        if (c > WRITE_CHUNK) c = WRITE_CHUNK;
        if (c > free_space) c = free_space;

        int w = uart_write_bytes(P4_UART_PORT, (const char *)(buf + done), c);
        if (w <= 0) {
            ESP_LOGE(TAG, "uart_write_bytes ret=%d", w);
            break;
        }
        done += w;
        last_write_progress:
        last_progress = xTaskGetTickCount();   /* 有进展，重置超时 */
    }
    return (int)done;
}