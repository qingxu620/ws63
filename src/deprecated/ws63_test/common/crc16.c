/**
 * @file crc16.c
 * @brief CRC16-CCITT 校验实现
 */
#include "crc16.h"
#include <string.h>

uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

void motion_cmd_set_crc(motion_cmd_t *cmd)
{
    cmd->crc16 = 0;
    cmd->crc16 = crc16_ccitt((const uint8_t *)cmd, sizeof(motion_cmd_t));
}

bool motion_cmd_check_crc(const motion_cmd_t *cmd)
{
    motion_cmd_t tmp;
    memcpy(&tmp, cmd, sizeof(motion_cmd_t));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(motion_cmd_t)) == recv_crc;
}

void status_pkt_set_crc(status_pkt_t *pkt)
{
    pkt->crc16 = 0;
    pkt->crc16 = crc16_ccitt((const uint8_t *)pkt, sizeof(status_pkt_t));
}

bool status_pkt_check_crc(const status_pkt_t *pkt)
{
    status_pkt_t tmp;
    memcpy(&tmp, pkt, sizeof(status_pkt_t));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(status_pkt_t)) == recv_crc;
}

void focus_cmd_set_crc(focus_node_cmd_t *cmd)
{
    cmd->crc16 = 0;
    cmd->crc16 = crc16_ccitt((const uint8_t *)cmd, sizeof(focus_node_cmd_t));
}

bool focus_cmd_check_crc(const focus_node_cmd_t *cmd)
{
    focus_node_cmd_t tmp;
    memcpy(&tmp, cmd, sizeof(focus_node_cmd_t));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(focus_node_cmd_t)) == recv_crc;
}

void focus_status_set_crc(focus_node_status_t *pkt)
{
    pkt->crc16 = 0;
    pkt->crc16 = crc16_ccitt((const uint8_t *)pkt, sizeof(focus_node_status_t));
}

bool focus_status_check_crc(const focus_node_status_t *pkt)
{
    focus_node_status_t tmp;
    memcpy(&tmp, pkt, sizeof(focus_node_status_t));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(focus_node_status_t)) == recv_crc;
}

void safety_cmd_set_crc(safety_node_cmd_t *cmd)
{
    cmd->crc16 = 0;
    cmd->crc16 = crc16_ccitt((const uint8_t *)cmd, sizeof(safety_node_cmd_t));
}

bool safety_cmd_check_crc(const safety_node_cmd_t *cmd)
{
    safety_node_cmd_t tmp;
    memcpy(&tmp, cmd, sizeof(safety_node_cmd_t));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(safety_node_cmd_t)) == recv_crc;
}

void safety_status_set_crc(safety_node_status_t *pkt)
{
    pkt->crc16 = 0;
    pkt->crc16 = crc16_ccitt((const uint8_t *)pkt, sizeof(safety_node_status_t));
}

bool safety_status_check_crc(const safety_node_status_t *pkt)
{
    safety_node_status_t tmp;
    memcpy(&tmp, pkt, sizeof(safety_node_status_t));
    uint16_t recv_crc = tmp.crc16;
    tmp.crc16 = 0;
    return crc16_ccitt((const uint8_t *)&tmp, sizeof(safety_node_status_t)) == recv_crc;
}
