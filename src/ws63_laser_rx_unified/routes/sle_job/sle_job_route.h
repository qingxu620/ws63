/**
 * @file sle_job_route.h
 * @brief Integrated RX SLE job route lifecycle.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_ROUTE_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_ROUTE_H

#include "errcode.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t sle_job_route_start(void);
bool sle_job_route_is_idle(void);
bool sle_job_route_is_server_ready(void);
bool sle_job_route_server_failed(void);
bool sle_job_route_is_connected(void);
void sle_job_route_force_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_ROUTE_H */
