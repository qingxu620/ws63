/**
 * @file dac8562.h
 * @brief DAC8562 双通道16位SPI DAC驱动 (WS63 移植版)
 *        消除了 Arduino 原始库的双重字节序反转问题
 */
#ifndef DAC8562_H
#define DAC8562_H

#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DAC8562 命令字节 */
#define DAC_CMD_SETA_UPDATEA 0x18 /* 写入并更新 DAC-A */
#define DAC_CMD_SETB_UPDATEB 0x19 /* 写入并更新 DAC-B */
#define DAC_CMD_UPDATE_ALL 0x0F   /* 更新所有 DAC */
#define DAC_CMD_GAIN 0x02         /* 设置增益 */
#define DAC_CMD_PWR_UP 0x20       /* 上电 */
#define DAC_CMD_RESET 0x28        /* 复位 */
#define DAC_CMD_LDAC_DIS 0x30     /* LDAC 直通 */
#define DAC_CMD_INT_REF_EN 0x38   /* 使能内部参考 */

/* 增益配置数据 */
#define DAC_GAIN_B2_A2 0x0000
#define DAC_GAIN_B2_A1 0x0001
#define DAC_GAIN_B1_A2 0x0002
#define DAC_GAIN_B1_A1 0x0003

/**
 * @brief  初始化 DAC8562 (SPI + GPIO 配置 + DAC 初始化序列)
 * @return ERRCODE_SUCC 成功
 */
errcode_t dac8562_init(void);
void dac8562_recover(void);

/**
 * @brief  向 DAC 写入一个命令+数据
 * @param  cmd   命令字节 (DAC_CMD_xxx)
 * @param  value 16位数据值
 */
void dac8562_write_channel(uint8_t cmd, uint16_t value);

/**
 * @brief  同时更新 X/Y 两个通道
 * @param  x_val X 通道 DAC 值 (0-65535)
 * @param  y_val Y 通道 DAC 值 (0-65535)
 */
void dac8562_write_xy(uint16_t x_val, uint16_t y_val);

#ifdef __cplusplus
}
#endif

#endif /* DAC8562_H */
