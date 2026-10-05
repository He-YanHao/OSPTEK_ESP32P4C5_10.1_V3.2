#include "hal_uart.h"
#include "driver/uart.h"
#include "esp_log.h"

/* P4 平台相关定义 */
#define P4_UART_PORT      UART_NUM_1
#define P4_UART_TX_PIN    53
#define P4_UART_RX_PIN    51
#define P4_UART_BAUD      921600
#define P4_UART_BUF_SIZE  (1024 * 32)

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
    ESP_ERROR_CHECK(uart_param_config(P4_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(P4_UART_PORT,
                                  P4_UART_TX_PIN,
                                  P4_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE,
                                  UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(P4_UART_PORT,
                                        P4_UART_BUF_SIZE,
                                        P4_UART_BUF_SIZE,
                                        0, NULL, 0));

    ESP_LOGI(TAG, "UART%d init: TX=%d RX=%d baud=%d",
             P4_UART_PORT, P4_UART_TX_PIN, P4_UART_RX_PIN, P4_UART_BAUD);
    s_uart_inited = 1;
    return 0;
}

int hal_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(P4_UART_PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
}

int hal_uart_write(const uint8_t *buf, size_t len)
{
    return uart_write_bytes(P4_UART_PORT, (const char *)buf, len);
}