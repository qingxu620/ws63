/**
 * @file packet.h
 * @brief Encode/decode helpers for SLE job packets.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_PACKET_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_PACKET_H

#include "sle_job_protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t len;
    const uint8_t *payload;
} sle_job_packet_view_t;

bool sle_job_packet_encode(uint8_t type, uint8_t flags, uint16_t seq,
                       const void *payload, uint16_t payload_len,
                       uint8_t *out, uint16_t out_cap, uint16_t *out_len);
bool sle_job_packet_decode(const uint8_t *data, uint16_t data_len, sle_job_packet_view_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_PACKET_H */
