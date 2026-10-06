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

/* ============================================================
 *  P4-C5 硬件连接定义
 * ============================================================
 * 如更换板子，只需改这里的引脚号。
 */
#define PIN_SPI_CS          17
#define PIN_SPI_MISO        14
#define PIN_SPI_MOSI        19
#define PIN_SPI_CLK         18
#define PIN_SPI_IRQ         15      /* C5 用来通知 P4 "有事件"的握手线 */
#define PIN_C5_CHIP_PU      54      /* C5 电源使能（高电平有效） */

/* SPI 时钟：400kHz
 * 说明：
 *   - 500kHz 曾稳定工作，但 400kHz 给 C5 更多时序余量
 *   - C5 在 WiFi 活跃时 CPU 很忙，SPI 从机响应可能延迟
 *   - 400kHz 是稳定与速度的折中 */
#define SPI_CLK_HZ          (400 * 1000)

/* 单次 SPI 事务最大长度（含 3 字节头） */
#define SPI_MAX_XFER_SZ     4096

/* 3 字节协议头：[CMD][ADDR][0xFF] */
#define SPI_HDR_LEN         3

/* 阈值：小于此长度用栈缓冲，避免小事务频繁 malloc */
#define SPI_STACK_BUF_MAX   64

static const char *TAG = "HAL_SPI";
static spi_device_handle_t s_spi_dev = NULL;
static SemaphoreHandle_t   s_irq_sem = NULL;   /* 计数信号量，最多累积 16 次 IRQ */
static int                 s_inited  = 0;

/*
 * GPIO IRQ 中断服务程序。
 * C5 拉高 IRQ 线时触发，只做一件事：给信号量（唤醒等待者）。
 * 不做任何 SPI 操作（IRAM 上下文，禁止耗时调用）。
 */
static void IRAM_ATTR gpio_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_irq_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/*
 * 初始化 SPI 主机 + IRQ + C5 电源引脚。
 * 幂等：第二次调用直接返回 0。
 * 返回：0 成功，-1 失败。
 */
int hal_spi_init(void)
{
    if (s_inited) return 0;

    /* 复位引脚，避免被其他外设占用 */
    gpio_reset_pin(PIN_SPI_CS);
    gpio_reset_pin(PIN_SPI_MISO);
    gpio_reset_pin(PIN_SPI_MOSI);
    gpio_reset_pin(PIN_SPI_CLK);
    gpio_reset_pin(PIN_SPI_IRQ);

    /* SPI 总线初始化 */
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

    /* SPI 从设备（C5）配置：Mode 0，CS 由硬件控制 */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz   = SPI_CLK_HZ,
        .mode             = 0,                  /* CPOL=0, CPHA=0 */
        .spics_io_num     = PIN_SPI_CS,
        .queue_size       = 3,
        .cs_ena_pretrans  = 4,                  /* CS 建立：4 个时钟周期 */
        .cs_ena_posttrans = 4,                  /* CS 保持：4 个时钟周期 */
    };
    if (spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device failed");
        return -1;
    }

    /* IRQ 引脚：输入 + 上拉，上升沿触发（C5 拉高表示有事件） */
    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << PIN_SPI_IRQ,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    if (gpio_config(&irq_cfg) != ESP_OK) return -1;

    /* 计数信号量：最多累积 16 个 IRQ，避免背靠背中断丢失 */
    s_irq_sem = xSemaphoreCreateCounting(16, 0);
    if (!s_irq_sem) return -1;

    /* 安装 GPIO ISR 服务（幂等，允许外部已安装） */
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(e));
        return -1;
    }
    if (gpio_isr_handler_add(PIN_SPI_IRQ, gpio_irq_isr, NULL) != ESP_OK) {
        return -1;
    }

    /* C5 电源引脚：输出，默认拉高使能 */
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

/*
 * 等 C5 的 IRQ。
 * timeout_ms = 0 时立即返回（非阻塞）。
 * 返回：0 收到；<0 超时。
 */
int hal_spi_wait_irq(uint32_t timeout_ms)
{
    if (!s_irq_sem) return -1;
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return (xSemaphoreTake(s_irq_sem, ticks) == pdTRUE) ? 0 : -1;
}

/*
 * 清空 IRQ 计数（复位 C5 后调用，避免旧信号残留）。
 */
void hal_spi_clear_irq(void)
{
    if (!s_irq_sem) return;
    while (xSemaphoreTake(s_irq_sem, 0) == pdTRUE) { /* drain */ }
}

/*
 * 单次 SPI 事务。
 *
 * 事务格式：[CMD][ADDR][0xFF] + payload
 * 整个事务在一次 CS 拉低内完成。
 *
 * 内存策略：
 *   - total <= 64：用栈缓冲（绝大多数控制命令，3+4=7 字节）
 *   - total >  64：用 heap_caps_malloc(DMA)（大块 RDDMA 读）
 *
 * DMA 对齐：
 *   栈缓冲加 WORD_ALIGNED_ATTR 保证 4 字节对齐
 *
 * 返回：0 成功；<0 失败。
 */
int hal_spi_transfer(uint8_t cmd, uint8_t addr,
                     const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_spi_dev) return -1;
    if (len + SPI_HDR_LEN > SPI_MAX_XFER_SZ) return -1;

    size_t total = SPI_HDR_LEN + len;

    /* 小事务走栈，避免频繁 malloc */
    WORD_ALIGNED_ATTR uint8_t stack_tx[SPI_STACK_BUF_MAX];
    WORD_ALIGNED_ATTR uint8_t stack_rx[SPI_STACK_BUF_MAX];
    uint8_t *tx_buf = stack_tx;
    uint8_t *rx_buf = rx ? stack_rx : NULL;
    bool     heap_alloc = false;

    /* 大事务走 DMA heap */
    if (total > SPI_STACK_BUF_MAX) {
        tx_buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_DMA);
        if (!tx_buf) return -1;
        if (rx) {
            rx_buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_DMA);
            if (!rx_buf) { heap_caps_free(tx_buf); return -1; }
        }
        heap_alloc = true;
    }

    /* 组装 3 字节头 + payload */
    tx_buf[0] = cmd;
    tx_buf[1] = addr;
    tx_buf[2] = 0xFF;   /* 协议规定的占位字节 */
    if (tx && len)      memcpy(tx_buf + SPI_HDR_LEN, tx, len);
    else if (len)       memset(tx_buf + SPI_HDR_LEN, 0xFF, len);

    /* 执行 SPI 事务（阻塞式，直到完成） */
    spi_transaction_t t = {
        .length    = total * 8,       /* SPI 长度单位是 bit */
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    esp_err_t ret = spi_device_transmit(s_spi_dev, &t);

    /* 把接收数据从 rx_buf 拷到调用者的 rx（去掉 3 字节头） */
    if (ret == ESP_OK && rx && rx_buf) {
        memcpy(rx, rx_buf + SPI_HDR_LEN, len);
    }

    /* 释放 heap 缓冲 */
    if (heap_alloc) {
        heap_caps_free(tx_buf);
        if (rx_buf) heap_caps_free(rx_buf);
    }
    return (ret == ESP_OK) ? 0 : -1;
}

/* C5 电源控制 */
void hal_c5_power_on(void)  { gpio_set_level(PIN_C5_CHIP_PU, 1); }
void hal_c5_power_off(void) { gpio_set_level(PIN_C5_CHIP_PU, 0); }

/*
 * C5 复位：拉低 PU 100ms 再拉高。
 * 复位后清空 IRQ 计数（避免上电瞬间的干扰信号污染）。
 */
void hal_c5_reset(void)
{
    gpio_set_level(PIN_C5_CHIP_PU, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_C5_CHIP_PU, 1);
    hal_spi_clear_irq();
}