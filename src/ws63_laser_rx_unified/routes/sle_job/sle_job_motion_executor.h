/**
 * @file motion_executor.h
 * @brief Local synchronous motion executor.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_MOTION_EXECUTOR_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_MOTION_EXECUTOR_H

#include "errcode.h"
#include "sle_job_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void sle_job_motion_executor_init(void);
errcode_t sle_job_motion_executor_start_task(void);
bool sle_job_motion_executor_enqueue(const sle_job_motion_cmd_t *cmd);
void sle_job_motion_executor_flush(void);
void sle_job_motion_executor_execute(const sle_job_motion_cmd_t *cmd);
void sle_job_motion_executor_set_origin(void);
void sle_job_motion_executor_request_abort(void);
void sle_job_motion_executor_clear_abort(void);
double sle_job_motion_executor_get_x(void);
double sle_job_motion_executor_get_y(void);
bool sle_job_motion_executor_is_busy(void);
uint16_t sle_job_motion_executor_queue_depth(void);
bool sle_job_motion_executor_worker_started(void);
bool sle_job_motion_executor_abort_requested(void);
unsigned long sle_job_motion_executor_enqueued_count(void);
unsigned long sle_job_motion_executor_executed_count(void);
unsigned long sle_job_motion_executor_last_activity_ms(void);
unsigned long sle_job_motion_executor_late_sample_count(void);
unsigned long sle_job_motion_executor_missed_sample_count(void);
unsigned long sle_job_motion_executor_motion_segment_count(void);
unsigned long sle_job_motion_executor_short_segment_count(void);
unsigned long sle_job_motion_executor_max_sample_late_us(void);
unsigned long sle_job_motion_executor_dac_write_count(void);
unsigned long sle_job_motion_executor_dac_skip_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_MOTION_EXECUTOR_H */
