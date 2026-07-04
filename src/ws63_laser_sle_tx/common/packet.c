/**
 * @file packet.c
 * @brief Encode/decode helpers for SLE job packets.
 */
#include "packet.h"
#include "crc16.h"
#include <string.h>

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool sle_packet_encode(uint8_t type, uint8_t flags, uint16_t seq,
                       const void *payload, uint16_t payload_len,
                       uint8_t *out, uint16_t out_cap, uint16_t *out_len)
{
    if (out == NULL || out_len == NULL || payload_len > SLE_JOB_PACKET_MAX_PAYLOAD ||
        out_cap < (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + payload_len)) {
        return false;
    }
    if (payload_len > 0 && payload == NULL) {
        return false;
    }

    put_le16(&out[0], SLE_JOB_PACKET_MAGIC);
    out[2] = type;
    out[3] = flags;
    put_le16(&out[4], seq);
    put_le16(&out[6], payload_len);
    put_le16(&out[8], 0);
    if (payload_len > 0) {
        (void)memcpy(&out[SLE_JOB_PACKET_HEADER_LEN], payload, payload_len);
    }

    uint16_t crc = job_crc16_ccitt(out, (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + payload_len));
    put_le16(&out[8], crc);
    *out_len = (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + payload_len);
    return true;
}

bool sle_packet_decode(const uint8_t *data, uint16_t data_len, sle_packet_view_t *out)
{
    if (data == NULL || out == NULL || data_len < SLE_JOB_PACKET_HEADER_LEN) {
        return false;
    }
    if (get_le16(&data[0]) != SLE_JOB_PACKET_MAGIC) {
        return false;
    }

    uint16_t payload_len = get_le16(&data[6]);
    if (payload_len > SLE_JOB_PACKET_MAX_PAYLOAD ||
        data_len < (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + payload_len)) {
        return false;
    }

    uint8_t tmp[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = (uint16_t)(SLE_JOB_PACKET_HEADER_LEN + payload_len);
    (void)memcpy(tmp, data, packet_len);
    uint16_t recv_crc = get_le16(&tmp[8]);
    put_le16(&tmp[8], 0);
    if (job_crc16_ccitt(tmp, packet_len) != recv_crc) {
        return false;
    }

    out->type = data[2];
    out->flags = data[3];
    out->seq = get_le16(&data[4]);
    out->len = payload_len;
    out->payload = (payload_len > 0) ? &data[SLE_JOB_PACKET_HEADER_LEN] : NULL;
    return true;
}
