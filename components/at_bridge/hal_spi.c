#include "hal_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "esp_attr.h"

#define PIN_SPI_CS          17
#define PIN_SPI_MISO        14
#define PIN_SPI_MOSI        19
#define PIN_SPI_CLK         18
#define PIN_SPI_IRQ         15
#define PIN_C5_CHIP_PU      54
#define SPI_CLK_HZ          (500 * 1000)
#define SPI_MAX_XFER_SZ     4096
#define SPI_HDR_LEN         3
#define SPI_STACK_BUF_MAX   64      /* 小于此长度用栈 buf，不 malloc */

static const char *TAG = "HAL_SPI";
static spi_device_handle_t s_spi_dev = NULL;
static SemaphoreHandle_t   s_irq_sem = NULL;
static int                 s_inited  = 0;

static void IRAM_ATTR gpio_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_irq_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

int hal_spi_init(void)
{
    if (s_inited) return 0;

    gpio_reset_pin(PIN_SPI_CS);
    gpio_reset_pin(PIN_SPI_MISO);
    gpio_reset_pin(PIN_SPI_MOSI);
    gpio_reset_pin(PIN_SPI_CLK);
    gpio_reset_pin(PIN_SPI_IRQ);

    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_SPI_MOSI,
        .miso_io_num     = PIN_SPI_MISO,
        .sclk_io_num     = PIN_SPI_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SPI_MAX_XFER_SZ,
    };
    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed");
        return -1;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz   = SPI_CLK_HZ,
        .mode             = 0,
        .spics_io_num     = PIN_SPI_CS,
        .queue_size       = 3,
        .cs_ena_pretrans  = 4,
        .cs_ena_posttrans = 4,
    };
    if (spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device failed");
        return -1;
    }

    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << PIN_SPI_IRQ,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    if (gpio_config(&irq_cfg) != ESP_OK) return -1;

    s_irq_sem = xSemaphoreCreateCounting(16, 0);
    if (!s_irq_sem) return -1;

    /* S5: gpio_install_isr_service 幂等，允许外部已安装 */
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(e));
        return -1;
    }
    if (gpio_isr_handler_add(PIN_SPI_IRQ, gpio_irq_isr, NULL) != ESP_OK) {
        return -1;
    }

    gpio_config_t pu_cfg = {
        .pin_bit_mask = 1ULL << PIN_C5_CHIP_PU,
        .mode         = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&pu_cfg) != ESP_OK) return -1;
    gpio_set_level(PIN_C5_CHIP_PU, 1);

    ESP_LOGI(TAG, "SPI init: CLK=%d MOSI=%d MISO=%d CS=%d IRQ=%d clk=%dkHz",
             PIN_SPI_CLK, PIN_SPI_MOSI, PIN_SPI_MISO,
             PIN_SPI_CS, PIN_SPI_IRQ, SPI_CLK_HZ / 1000);
    s_inited = 1;
    return 0;
}

int hal_spi_wait_irq(uint32_t timeout_ms)
{
    if (!s_irq_sem) return -1;
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return (xSemaphoreTake(s_irq_sem, ticks) == pdTRUE) ? 0 : -1;
}

void hal_spi_clear_irq(void)
{
    if (!s_irq_sem) return;
    while (xSemaphoreTake(s_irq_sem, 0) == pdTRUE) { }
}

/*
 * S4: 小事务走栈 buf（绝大数情况：3 字节头 + 4 字节 payload = 7）
 *     大事务走 heap_caps_malloc(DMA)，避免堆碎片干扰非 DMA 分配
 */
int hal_spi_transfer(uint8_t cmd, uint8_t addr,
                     const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_spi_dev) return -1;
    if (len + SPI_HDR_LEN > SPI_MAX_XFER_SZ) return -1;

    size_t total = SPI_HDR_LEN + len;

    /* ★ DMA 对齐：栈缓冲加 WORD_ALIGNED_ATTR */
    WORD_ALIGNED_ATTR uint8_t stack_tx[SPI_STACK_BUF_MAX];
    WORD_ALIGNED_ATTR uint8_t stack_rx[SPI_STACK_BUF_MAX];
    uint8_t *tx_buf = stack_tx;
    uint8_t *rx_buf = rx ? stack_rx : NULL;
    bool     heap_alloc = false;

    if (total > SPI_STACK_BUF_MAX) {
        tx_buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_DMA);
        if (!tx_buf) return -1;
        if (rx) {
            rx_buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_DMA);
            if (!rx_buf) { heap_caps_free(tx_buf); return -1; }
        }
        heap_alloc = true;
    }

    tx_buf[0] = cmd;
    tx_buf[1] = addr;
    tx_buf[2] = 0xFF;   /* 协议占位字节 */
    if (tx && len)      memcpy(tx_buf + SPI_HDR_LEN, tx, len);
    else if (len)       memset(tx_buf + SPI_HDR_LEN, 0xFF, len);

    spi_transaction_t t = {
        .length    = total * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    esp_err_t ret = spi_device_transmit(s_spi_dev, &t);
    if (ret == ESP_OK && rx && rx_buf) {
        memcpy(rx, rx_buf + SPI_HDR_LEN, len);
    }

    if (heap_alloc) {
        heap_caps_free(tx_buf);
        if (rx_buf) heap_caps_free(rx_buf);
    }
    return (ret == ESP_OK) ? 0 : -1;
}

void hal_c5_power_on(void)  { gpio_set_level(PIN_C5_CHIP_PU, 1); }
void hal_c5_power_off(void) { gpio_set_level(PIN_C5_CHIP_PU, 0); }

void hal_c5_reset(void)
{
    gpio_set_level(PIN_C5_CHIP_PU, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_C5_CHIP_PU, 1);
    hal_spi_clear_irq();
}