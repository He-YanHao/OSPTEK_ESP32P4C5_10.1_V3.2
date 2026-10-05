#include "hal_uart.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define P4_UART_PORT      UART_NUM_1
#define P4_UART_TX_PIN    53
#define P4_UART_RX_PIN    51
#define P4_UART_BAUD      230400
#define P4_UART_RX_BUF    (8 * 1024)
#define P4_UART_TX_BUF    (32 * 1024)

#define WRITE_CHUNK       256
#define WRITE_STALL_MS    500

static const char *TAG = "HAL_UART";
static int s_uart_inited = 0;

int hal_uart_init(void)
{
    if (s_uart_inited) return 0;

    uart_config_t cfg = {
        .baud_rate  = P4_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
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

int hal_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(P4_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
}

/*
 * 契约：
 *   = len  全部写完
 *   < len  部分写完（无进展超时或错误）
 *   < 0    无法写入
 * 不主动丢弃数据；TX 满时 vTaskDelay 等，500ms 无进展则返回部分。
 */
int hal_uart_write(const uint8_t *buf, size_t len)
{
    size_t done = 0;
    TickType_t last_progress = xTaskGetTickCount();
    const TickType_t stall_to = pdMS_TO_TICKS(WRITE_STALL_MS);

    while (done < len) {
        size_t free_space = 0;
        if (uart_get_tx_buffer_free_size(P4_UART_PORT, &free_space) != ESP_OK) {
            free_space = 32;   /* 保守 fallback */
        }

        if (free_space < 32) {
            if ((xTaskGetTickCount() - last_progress) >= stall_to) {
                ESP_LOGW(TAG, "uart_write stalled: %u/%u",
                         (unsigned)done, (unsigned)len);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t c = len - done;
        if (c > WRITE_CHUNK) c = WRITE_CHUNK;
        if (c > free_space) c = free_space;

        int w = uart_write_bytes(P4_UART_PORT, (const char *)(buf + done), c);
        if (w <= 0) {
            ESP_LOGE(TAG, "uart_write_bytes ret=%d", w);
            break;
        }
        done += w;
        last_progress = xTaskGetTickCount();
    }
    return (int)done;
}