/**
 * @file sle_job_server.h
 * @brief SLE server used by RX to receive structured job packets.
 */
#ifndef WS63_LASER_SLE_JOB_SERVER_H
#define WS63_LASER_SLE_JOB_SERVER_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sle_job_packet_rx_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*sle_job_disconnect_cb_t)(void);

errcode_t sle_job_server_init(void);
bool sle_job_server_is_connected(void);
errcode_t sle_job_server_send_packet(const void *data, uint16_t len);
const char *sle_job_server_get_status(void);
void sle_job_server_set_packet_cb(sle_job_packet_rx_cb_t cb);
void sle_job_server_set_disconnect_cb(sle_job_disconnect_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_SLE_JOB_SERVER_H */
