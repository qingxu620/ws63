/**
 * @file crc16.c
 * @brief CRC16-CCITT implementation for job packets.
 */
#include "sle_job_crc16.h"

uint16_t sle_job_crc16_ccitt_update(uint16_t crc, const uint8_t *data, uint16_t len)
{
    if (data == 0 || len == 0) {
        return crc;
    }

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000U) != 0) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t sle_job_crc16_ccitt(const uint8_t *data, uint16_t len)
{
    return sle_job_crc16_ccitt_update(0xFFFFU, data, len);
}
