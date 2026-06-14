/**
 * @file motion_executor.h
 * @brief Local synchronous motion executor.
 */
#ifndef MOTION_EXECUTOR_H
#define MOTION_EXECUTOR_H

#include "errcode.h"
#include "protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void motion_executor_init(void);
errcode_t motion_executor_start_task(void);
bool motion_executor_enqueue(const motion_cmd_t *cmd);
bool motion_executor_enqueue_deferred(const motion_cmd_t *cmd);
void motion_executor_signal_worker(void);
void motion_executor_flush(void);
void motion_executor_execute(const motion_cmd_t *cmd);
void motion_executor_set_origin(void);
void motion_executor_request_abort(void);
double motion_executor_get_x(void);
double motion_executor_get_y(void);
bool motion_executor_is_busy(void);
uint16_t motion_executor_queue_depth(void);
bool motion_executor_worker_started(void);
bool motion_executor_queue_ready(void);
bool motion_executor_abort_requested(void);
unsigned long motion_executor_enqueued_count(void);
unsigned long motion_executor_executed_count(void);
unsigned long motion_executor_last_activity_ms(void);
unsigned long motion_executor_late_sample_count(void);
unsigned long motion_executor_missed_sample_count(void);
unsigned long motion_executor_motion_segment_count(void);
unsigned long motion_executor_short_segment_count(void);
unsigned long motion_executor_max_sample_late_us(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_EXECUTOR_H */
