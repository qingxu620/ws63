/**
 * @file wireless_crc16.h
 * @brief CRC16 helpers for SLE packets.
 */
#ifndef LASER_WIRELESS_CRC16_H
#define LASER_WIRELESS_CRC16_H

#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

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

#endif /* LASER_WIRELESS_CRC16_H */
