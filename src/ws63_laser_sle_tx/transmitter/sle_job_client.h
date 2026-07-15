/**
 * @file sle_job_client.h
 * @brief SLE client used by TX to send structured job packets to RX.
 */
#ifndef WS63_LASER_SLE_JOB_CLIENT_H
#define WS63_LASER_SLE_JOB_CLIENT_H

#include "errcode.h"
#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sle_job_response_cb_t)(const uint8_t *data, uint16_t length);

errcode_t sle_job_client_init(void);
errcode_t sle_job_client_send_packet(const void *data, uint16_t len);
errcode_t sle_job_client_send_packet_ex(const void *data, uint16_t len, bool force_write_req);
errcode_t sle_job_client_send_packet_ex_timeout(const void *data, uint16_t len,
                                                bool force_write_req, uint32_t cfm_timeout_ms);
errcode_t sle_job_client_mirror_panel_packet(const void *data, uint16_t len);
bool sle_job_client_is_connected(void);
bool sle_job_client_panel_is_connected(void);
bool sle_job_client_panel_link_allowed(void);
void sle_job_client_set_panel_link_allowed(bool allowed);
void sle_job_client_set_background_seek_allowed(bool allowed);
void sle_job_client_poll_connect(void);
void sle_job_client_poll_link_diagnostics(void);
const char *sle_job_client_get_status(void);
void sle_job_client_set_response_cb(sle_job_response_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_SLE_JOB_CLIENT_H */
