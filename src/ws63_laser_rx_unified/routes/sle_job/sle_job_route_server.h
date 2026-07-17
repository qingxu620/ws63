/**
 * @file sle_job_server.h
 * @brief SLE server used by RX to receive structured job packets.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_ROUTE_SERVER_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_ROUTE_SERVER_H

#include "errcode.h"
#include "sle_job_protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sle_job_route_packet_rx_cb_t)(uint16_t conn_id, const uint8_t *data, uint16_t len);
typedef void (*sle_job_route_disconnect_cb_t)(void);

typedef struct {
    uint32_t callback_count;
    uint32_t callback_slow_count;
    uint32_t max_callback_gap_ms;
    uint32_t max_callback_ms;
    uint32_t work_count;
    uint32_t work_slow_count;
    uint32_t max_callback_to_work_ms;
    uint32_t max_work_wait_ms;
    uint32_t max_work_process_ms;
    uint32_t notify_count;
    uint32_t notify_fail_count;
    uint32_t notify_slow_count;
    uint32_t max_notify_ms;
    uint32_t work_dropped;
    uint8_t work_max_used;
} sle_job_route_diag_t;

errcode_t sle_job_route_server_init(void);
errcode_t sle_job_route_server_stop(void);
errcode_t sle_job_route_server_set_discoverable(bool enabled, const char *reason);
bool sle_job_route_server_is_connected(void);
errcode_t sle_job_route_server_send_packet(const void *data, uint16_t len);
errcode_t sle_job_route_server_broadcast_packet(const void *data, uint16_t len);
errcode_t sle_job_route_server_update_panel_status_adv(const sle_job_panel_status_payload_t *status);
uint16_t sle_job_route_server_get_owner_conn_id(void);
uint8_t sle_job_route_server_get_connection_count(void);
const char *sle_job_route_server_get_status(void);
void sle_job_route_server_set_packet_cb(sle_job_route_packet_rx_cb_t cb);
void sle_job_route_server_set_disconnect_cb(sle_job_route_disconnect_cb_t cb);
void sle_job_route_server_reset_diag(void);
void sle_job_route_server_get_diag(sle_job_route_diag_t *diag);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_ROUTE_SERVER_H */
