/**
 * @file sle_client.h
 * @brief Minimal SLE client for the wireless laser transmitter.
 */
#ifndef LASER_WIRELESS_TX_SLE_CLIENT_H
#define LASER_WIRELESS_TX_SLE_CLIENT_H

#include "errcode.h"
#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t sle_laser_client_init(void);
errcode_t sle_laser_client_send_cmd(const motion_cmd_t *cmd);
errcode_t sle_laser_client_send_cmd_no_cfm(const motion_cmd_t *cmd);
void sle_laser_client_note_business_activity(void);
bool sle_laser_client_is_connected(void);
bool sle_laser_client_has_handles_ready(void);
bool sle_laser_client_has_status_rx(void);
bool sle_laser_client_can_send_heartbeat(void);
bool sle_laser_client_is_ready(void);
bool sle_laser_client_wait_write_idle(uint32_t timeout_ms);
uint8_t sle_laser_client_get_remote_status(void);
void sle_laser_client_get_feedback_snapshot(uint8_t *status, double *x, double *y);
uint8_t sle_laser_client_get_queue_free(void);
uint16_t sle_laser_client_get_last_ack_seq(void);
bool sle_laser_client_wait_ack(uint16_t seq, uint32_t timeout_ms);
uint16_t sle_laser_client_get_cmd_handle(void);
uint16_t sle_laser_client_get_status_handle(void);
uint16_t sle_laser_client_get_pending_writes(void);
uint32_t sle_laser_client_get_last_business_write_ms(void);
uint32_t sle_laser_client_get_write_req_count(void);
uint32_t sle_laser_client_get_write_cfm_ok_count(void);
uint32_t sle_laser_client_get_write_cfm_fail_count(void);
uint32_t sle_laser_client_get_write_submit_fail_count(void);
uint32_t sle_laser_client_get_notify_rx_count(void);
uint32_t sle_laser_client_get_status_age_ms(void);
void sle_laser_client_force_clear_pending(void);
uint32_t sle_laser_client_get_force_clear_count(void);
void sle_laser_client_run_pending_watchdog(void);

#ifdef __cplusplus
}
#endif

#endif /* LASER_WIRELESS_TX_SLE_CLIENT_H */
