/**
 * @file dac8563.h
 * @brief DAC8563 dual-channel SPI DAC driver.
 */
#ifndef DAC8563_H
#define DAC8563_H

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

errcode_t dac8563_init(void);
errcode_t dac8563_recover(void);
errcode_t dac8563_write_channel(uint8_t cmd, uint16_t value);
errcode_t dac8563_write_xy(uint16_t x_val, uint16_t y_val);

#ifdef __cplusplus
}
#endif

#endif /* DAC8563_H */
