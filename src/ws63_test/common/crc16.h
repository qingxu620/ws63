/**
 * @file crc16.h
 * @brief CRC16-CCITT 校验
 */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  计算 CRC16-CCITT
 * @param  data 数据指针
 * @param  len  数据长度
 * @return CRC16 值
 */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);

/**
 * @brief  为运动命令包设置 CRC16
 * @param  cmd 命令包指针 (crc16 字段会被填充)
 */
void motion_cmd_set_crc(motion_cmd_t *cmd);

/**
 * @brief  校验运动命令包的 CRC16
 * @param  cmd 命令包指针
 * @return true=校验通过, false=校验失败
 */
bool motion_cmd_check_crc(const motion_cmd_t *cmd);

/**
 * @brief  为状态反馈包设置 CRC16
 */
void status_pkt_set_crc(status_pkt_t *pkt);

/**
 * @brief  校验状态反馈包的 CRC16
 */
bool status_pkt_check_crc(const status_pkt_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* CRC16_H */
