/**
 * @file crc16.h
 * @brief CRC16-CCITT helpers for the internal SLE protocol.
 */
#ifndef WS63_SLE_LASER_CRC16_H
#define WS63_SLE_LASER_CRC16_H

#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);
void motion_cmd_set_crc(motion_cmd_t *cmd);
bool motion_cmd_check_crc(const motion_cmd_t *cmd);
void status_pkt_set_crc(status_pkt_t *pkt);
bool status_pkt_check_crc(const status_pkt_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* WS63_SLE_LASER_CRC16_H */
