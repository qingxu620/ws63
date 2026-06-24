/**
 * @file job_manager.h
 * @brief Structured job protocol state machine on the RX board.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_MANAGER_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sle_job_manager_init(void);
void sle_job_manager_on_packet(uint16_t conn_id, const uint8_t *data, uint16_t len);
void sle_job_manager_on_disconnect(void);
void sle_job_manager_safe_stop(const char *reason);
bool sle_job_manager_is_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_MANAGER_H */
