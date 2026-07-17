/**
 * @file motion_executor.h
 * @brief Local synchronous motion executor.
 */
#ifndef LEGACY_WIFI_MOTION_EXECUTOR_H
#define LEGACY_WIFI_MOTION_EXECUTOR_H

#include "errcode.h"
#include "legacy_wifi_motion_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void legacy_wifi_motion_executor_init(void);
errcode_t legacy_wifi_motion_executor_start_task(void);
bool legacy_wifi_motion_executor_enqueue(const legacy_wifi_motion_cmd_t *cmd);
void legacy_wifi_motion_executor_flush(void);
void legacy_wifi_motion_executor_execute(const legacy_wifi_motion_cmd_t *cmd);
void legacy_wifi_motion_executor_set_origin(void);
void legacy_wifi_motion_executor_request_abort(void);
double legacy_wifi_motion_executor_get_x(void);
double legacy_wifi_motion_executor_get_y(void);
bool legacy_wifi_motion_executor_is_busy(void);
uint16_t legacy_wifi_motion_executor_queue_depth(void);
bool legacy_wifi_motion_executor_worker_started(void);
bool legacy_wifi_motion_executor_abort_requested(void);
unsigned long legacy_wifi_motion_executor_enqueued_count(void);
unsigned long legacy_wifi_motion_executor_executed_count(void);
unsigned long legacy_wifi_motion_executor_last_activity_ms(void);
unsigned long legacy_wifi_motion_executor_late_sample_count(void);
unsigned long legacy_wifi_motion_executor_missed_sample_count(void);
unsigned long legacy_wifi_motion_executor_motion_segment_count(void);
unsigned long legacy_wifi_motion_executor_short_segment_count(void);
unsigned long legacy_wifi_motion_executor_max_sample_late_us(void);
unsigned long legacy_wifi_motion_executor_dac_skip_count(void);
unsigned long legacy_wifi_motion_executor_timer_wait_count(void);
unsigned long legacy_wifi_motion_executor_timer_fail_count(void);
unsigned long legacy_wifi_motion_executor_timer_wake_late_max_us(void);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_WIFI_MOTION_EXECUTOR_H */
