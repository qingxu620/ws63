/**
 * @file sle_job_client.h
 * @brief SLE client used by TX to send structured job packets to RX.
 */
#ifndef WS63_LASER_SLE_JOB_CLIENT_H
#define WS63_LASER_SLE_JOB_CLIENT_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sle_job_response_cb_t)(const uint8_t *data, uint16_t length);

errcode_t sle_job_client_init(void);
errcode_t sle_job_client_send_packet(const void *data, uint16_t len);
bool sle_job_client_is_connected(void);
void sle_job_client_poll_connect(void);
const char *sle_job_client_get_status(void);
void sle_job_client_set_response_cb(sle_job_response_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_SLE_JOB_CLIENT_H */
