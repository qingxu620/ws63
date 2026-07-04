/**
 * @file job_manager.h
 * @brief Structured job protocol state machine on the RX board.
 */
#ifndef WS63_LASER_SLE_JOB_MANAGER_H
#define WS63_LASER_SLE_JOB_MANAGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void job_manager_init(void);
void job_manager_on_packet(const uint8_t *data, uint16_t len);
void job_manager_on_disconnect(void);
void job_manager_safe_stop(const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_SLE_JOB_MANAGER_H */
