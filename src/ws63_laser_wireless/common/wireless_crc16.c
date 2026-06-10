/**
 * @file wireless_crc16.c
 * @brief CRC16-CCITT implementation for SLE packets.
 */
#include "wireless_crc16.h"
#include <string.h>

uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if ((crc & 0x8000) != 0) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

void motion_cmd_set_crc(motion_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    cmd->crc16 = 0;
    cmd->crc16 = crc16_ccitt((const uint8_t *)cmd, sizeof(motion_cmd_t));
}

bool motion_cmd_check_crc(const motion_cmd_t *cmd)
{
    if (cmd == NULL) {
        return false;
    }

    motion_cmd_t tmp;
    (void)memcpy(&tmp, cmd, sizeof(tmp));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(tmp)) == recv_crc;
}

void status_pkt_set_crc(status_pkt_t *pkt)
{
    if (pkt == NULL) {
        return;
    }

    pkt->crc16 = 0;
    pkt->crc16 = crc16_ccitt((const uint8_t *)pkt, sizeof(status_pkt_t));
}

bool status_pkt_check_crc(const status_pkt_t *pkt)
{
    if (pkt == NULL) {
        return false;
    }

    status_pkt_t tmp;
    (void)memcpy(&tmp, pkt, sizeof(tmp));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(tmp)) == recv_crc;
}
