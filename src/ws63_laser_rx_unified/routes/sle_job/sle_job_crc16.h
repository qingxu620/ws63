/**
 * @file crc16.h
 * @brief CRC16-CCITT helpers for job packets.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_CRC16_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_CRC16_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t sle_job_crc16_ccitt_update(uint16_t crc, const uint8_t *data, uint16_t len);
uint16_t sle_job_crc16_ccitt(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_CRC16_H */
