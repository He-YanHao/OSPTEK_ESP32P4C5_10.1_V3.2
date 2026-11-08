/*
 * Stage S1: SC2336 SCCB (I2C) liveness probe.
 *
 * Board facts taken from the baseboard schematic V1.3:
 *   - SCCB is NOT on dedicated pins. CSI_SDA_1V8 / CSI_SCL_1V8 go through
 *     U5 (TXS0102DCUR, VCCA=1V8, VCCB=+3V3, OE tied to +3V3) and land on
 *     I2C_SDA_GPIO7 / I2C_SCL_GPIO8, i.e. the SAME I2C0 bus as the touch
 *     panel (touch is behind R7/R8 0R, camera behind U5).
 *   - CSI_RST is driven by GPIO26 through R4 0R, pulled up by R20 10K to
 *     1V8, with C5 0.1uF to GND. The schematic note says the reset pin is a
 *     1.8 V domain input, so GPIO26 MUST be open-drain (never push 3.3 V).
 *   - MCLK comes from the on-board X1 24 MHz oscillator (R5 0R -> J1.14),
 *     so the P4 does not need to output XCLK.
 *
 * This stage only proves that the sensor answers on SCCB and reports a
 * plausible sensor ID. It does not touch CSI / ISP / V4L2.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sda_gpio;  /* SCCB SDA (3.3 V side), default CONFIG_APP_CAM_SCCB_SDA_GPIO */
    int scl_gpio;  /* SCCB SCL (3.3 V side), default CONFIG_APP_CAM_SCCB_SCL_GPIO */
    int rst_gpio;  /* camera reset, open-drain, default CONFIG_APP_CAM_RST_GPIO   */
} cam_sccb_cfg_t;

/**
 * @brief Reset the sensor and probe it over SCCB.
 *
 * Pass NULL to use the Kconfig defaults. If no device answers with the
 * configured SDA/SCL assignment, the two GPIOs are swapped once and the bus
 * is scanned again -- that removes any doubt about the hand-drawn polarity
 * of the level shifter without needing a second build.
 *
 * @return ESP_OK when the SC2336 sensor ID (0xcb3a) was read back.
 */
esp_err_t cam_sccb_probe(const cam_sccb_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
