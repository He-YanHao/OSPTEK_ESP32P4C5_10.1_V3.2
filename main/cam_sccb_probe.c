#include "cam_sccb_probe.h"

#include <stdbool.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sccb";

#define SCCB_IO_TIMEOUT_MS 50
#define SCCB_SCL_HZ        100000
#define SCCB_SCAN_FIRST    0x08
#define SCCB_SCAN_LAST     0x77
#define SCCB_MAX_FOUND     8

/* SC2336 sensor ID, datasheet table 1-5: 0x3107 = 0xcb, 0x3108 = 0x3a. */
#define SC2336_REG_PID_H   0x3107
#define SC2336_REG_PID_L   0x3108
#define SC2336_PID         0xcb3a

typedef struct {
    int sda;
    int scl;
    int rst;
} sccb_pins_t;

/* CSI_RST is pulled up to 1.8 V by R20 10K, so the sensor leaves reset on its
 * own. The GPIO is only pulsed when a first passive scan found nothing, and it
 * is always open-drain: the reset pin is a 1.8 V input and the board note says
 * "IO 使用开漏输出". Push-pull high would put 3.3 V on it. */
static void sccb_hw_reset(int rst_gpio)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << rst_gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "reset GPIO%d config failed, skipping pulse", rst_gpio);
        return;
    }

    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_gpio, 1);   /* release: high-Z, R20 pulls up to 1.8 V */
    vTaskDelay(pdMS_TO_TICKS(30));
}

static esp_err_t sccb_read_reg(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *val)
{
    const uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };
    return i2c_master_transmit_receive(dev, addr, sizeof(addr), val, 1,
                                       SCCB_IO_TIMEOUT_MS);
}

static int sccb_scan(i2c_master_bus_handle_t bus, uint8_t *found)
{
    int n = 0;
    for (int addr = SCCB_SCAN_FIRST; addr <= SCCB_SCAN_LAST && n < SCCB_MAX_FOUND; addr++) {
        if (i2c_master_probe(bus, addr, SCCB_IO_TIMEOUT_MS) == ESP_OK) {
            found[n++] = (uint8_t)addr;
        }
    }
    return n;
}

/* One bus attempt with a fixed SDA/SCL assignment. */
static esp_err_t sccb_try(const sccb_pins_t *pins, bool verbose)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)pins->sda,
        .scl_io_num = (gpio_num_t)pins->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  /* R51/R52 10K to +3V3 on board */
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed (SDA=%d SCL=%d): %s", pins->sda, pins->scl,
                 esp_err_to_name(err));
        return err;
    }

    uint8_t found[SCCB_MAX_FOUND] = {0};
    const int n = sccb_scan(bus, found);

    if (n == 0) {
        if (verbose) {
            ESP_LOGW(TAG, "SDA=GPIO%d SCL=GPIO%d: nobody acked", pins->sda, pins->scl);
        }
        i2c_del_master_bus(bus);
        return ESP_ERR_NOT_FOUND;
    }

    char list[64] = {0};
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        off += snprintf(list + off, sizeof(list) - off, "%s0x%02x", i ? " " : "", found[i]);
    }
    ESP_LOGI(TAG, "SDA=GPIO%d SCL=GPIO%d -> %d device(s): %s", pins->sda, pins->scl, n, list);

    bool pid_ok = false;
    for (int i = 0; i < n; i++) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = found[i],
            .scl_speed_hz = SCCB_SCL_HZ,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
            continue;
        }

        uint8_t hi = 0, lo = 0;
        if (sccb_read_reg(dev, SC2336_REG_PID_H, &hi) == ESP_OK &&
            sccb_read_reg(dev, SC2336_REG_PID_L, &lo) == ESP_OK) {
            const uint16_t pid = ((uint16_t)hi << 8) | lo;
            if (pid == SC2336_PID) {
                ESP_LOGI(TAG, "  0x%02x: PID=0x%04x  <-- SC2336 matched", found[i], pid);
                pid_ok = true;
            } else {
                ESP_LOGW(TAG, "  0x%02x: PID=0x%04x (expect 0x%04x, not an SC2336?)",
                         found[i], pid, SC2336_PID);
            }
        } else {
            ESP_LOGW(TAG, "  0x%02x: acked but PID read failed", found[i]);
        }
        i2c_master_bus_rm_device(dev);
    }

    i2c_del_master_bus(bus);
    return pid_ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Proof that the reset GPIO really is CSI_RST: assert it and the sensor must
 * disappear from the bus; release it and it must come back. This is a positive
 * test, so a mis-wired or mis-read schematic pin shows up instead of silently
 * "working". Costs two extra bus scans (~ms each) once per boot. */
static void cam_sccb_reset_selftest(int rst_gpio, int sda, int scl)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << rst_gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "reset self-test skipped: GPIO%d config failed", rst_gpio);
        return;
    }

    const sccb_pins_t pins = { .sda = sda, .scl = scl, .rst = rst_gpio };

    gpio_set_level(rst_gpio, 0);          /* assert reset */
    vTaskDelay(pdMS_TO_TICKS(50));
    const bool silent = sccb_try(&pins, false) != ESP_OK;

    gpio_set_level(rst_gpio, 1);          /* release */
    vTaskDelay(pdMS_TO_TICKS(50));
    const bool back = sccb_try(&pins, false) == ESP_OK;

    if (silent && back) {
        ESP_LOGI(TAG, "reset self-test PASS: GPIO%d low silences the sensor, release revives it",
                 rst_gpio);
    } else if (!silent) {
        ESP_LOGW(TAG, "reset self-test INCONCLUSIVE: GPIO%d low did NOT silence the sensor "
                      "(wrong pin, or reset not wired through R4)", rst_gpio);
    } else {
        ESP_LOGE(TAG, "reset self-test FAIL: sensor stayed silent after release -- check "
                      "power / MCLK on J1");
    }
}

esp_err_t cam_sccb_probe(const cam_sccb_cfg_t *cfg)
{
    sccb_pins_t pins = {
        .sda = cfg ? cfg->sda_gpio : CONFIG_APP_CAM_SCCB_SDA_GPIO,
        .scl = cfg ? cfg->scl_gpio : CONFIG_APP_CAM_SCCB_SCL_GPIO,
        .rst = cfg ? cfg->rst_gpio : CONFIG_APP_CAM_RST_GPIO,
    };

    ESP_LOGI(TAG, "S1: SCCB SDA=GPIO%d SCL=GPIO%d, reset=GPIO%d (open-drain, only on retry)",
             pins.sda, pins.scl, pins.rst);

    /* 1) Passive scan first: R20 already pulls CSI_RST to 1.8 V, so the sensor
     *    should answer without anyone touching the reset line. */
    if (sccb_try(&pins, false) == ESP_OK) {
        cam_sccb_reset_selftest(pins.rst, pins.sda, pins.scl);
        return ESP_OK;
    }

    /* 2) Pulse the reset line and retry. */
    ESP_LOGW(TAG, "no answer, pulsing reset on GPIO%d", pins.rst);
    sccb_hw_reset(pins.rst);
    if (sccb_try(&pins, true) == ESP_OK) {
        return ESP_OK;
    }

    /* 3) Same two steps with SDA/SCL swapped: the shifter polarity is easy to
     *    misread from a schematic, and a swap costs one extra scan. */
    ESP_LOGW(TAG, "retrying with SDA/SCL swapped");
    const sccb_pins_t swapped = { .sda = pins.scl, .scl = pins.sda, .rst = pins.rst };
    if (sccb_try(&swapped, false) == ESP_OK) {
        ESP_LOGW(TAG, "FOUND with swapped pins: use SDA=GPIO%d SCL=GPIO%d", swapped.sda,
                 swapped.scl);
        return ESP_OK;
    }
    sccb_hw_reset(swapped.rst);
    if (sccb_try(&swapped, true) == ESP_OK) {
        ESP_LOGW(TAG, "FOUND with swapped pins + reset: use SDA=GPIO%d SCL=GPIO%d",
                 swapped.sda, swapped.scl);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "S1 FAILED: no SC2336 on either pin order (with and without reset)");
    return ESP_ERR_NOT_FOUND;
}
