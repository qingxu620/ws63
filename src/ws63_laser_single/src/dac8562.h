/**
 * @file dac8562.h
 * @brief DAC8562 dual-channel SPI DAC driver.
 */
#ifndef DAC8562_H
#define DAC8562_H

#include "errcode.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAC_CMD_SETA_UPDATEA 0x18
#define DAC_CMD_SETB_UPDATEB 0x19
#define DAC_CMD_UPDATE_ALL 0x0F
#define DAC_CMD_GAIN 0x02
#define DAC_CMD_PWR_UP 0x20
#define DAC_CMD_RESET 0x28
#define DAC_CMD_LDAC_DIS 0x30
#define DAC_CMD_INT_REF_EN 0x38

#define DAC_GAIN_B2_A2 0x0000
#define DAC_GAIN_B2_A1 0x0001
#define DAC_GAIN_B1_A2 0x0002
#define DAC_GAIN_B1_A1 0x0003

errcode_t dac8562_init(void);
void dac8562_write_channel(uint8_t cmd, uint16_t value);
void dac8562_write_xy(uint16_t x_val, uint16_t y_val);

#ifdef __cplusplus
}
#endif

#endif /* DAC8562_H */

