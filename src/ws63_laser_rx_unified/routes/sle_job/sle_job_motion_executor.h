/**
 * @file motion_executor.h
 * @brief Local synchronous motion executor.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_MOTION_EXECUTOR_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_MOTION_EXECUTOR_H

#include "errcode.h"
#include "sle_job_protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long dac_write_count;
    unsigned long dac_skip_count;
    unsigned long wait_call_count;
    unsigned long late_sample_count;
    unsigned long missed_sample_count;
    unsigned long max_sample_late_us;
    unsigned long sched_relief_count;
    unsigned long timer_wait_count;
    unsigned long timer_fail_count;
    unsigned long timer_callback_count;
    unsigned long timer_callback_missing_count;
    unsigned long short_clamped_count;
    unsigned long deadline_catchup_count;
    unsigned long queue_empty_count;
    uint64_t dac_total_us;
    uint64_t wait_total_us;
    uint64_t planned_motion_us;
    uint64_t actual_motion_us;
    uint64_t short_clamped_added_us;
    uint64_t deadline_late_total_us;
    uint64_t timer_start_total_us;
    uint64_t timer_block_total_us;
    uint64_t timer_wake_late_total_us;
    uint64_t deadline_reset_discarded_us;
    uint32_t late_histogram[6];
    uint32_t timer_wake_late_histogram[6];
    uint32_t dac_max_us;
    uint32_t wait_max_us;
    uint32_t min_planned_step_us;
    uint32_t max_planned_step_us;
    uint32_t min_dac_gap_us;
    uint32_t max_dac_gap_us;
    uint32_t timer_wait_max_us;
    uint32_t timer_wake_late_max_us;
    unsigned long deadline_reset_count;
    uint16_t queue_min_depth;
    uint16_t queue_max_depth;
    uint16_t queue_avg_depth;
} sle_job_motion_diag_t;

void sle_job_motion_executor_init(void);
void sle_job_motion_executor_reset_stats(void);
errcode_t sle_job_motion_executor_start_task(void);
bool sle_job_motion_executor_enqueue(const sle_job_motion_cmd_t *cmd);
bool sle_job_motion_executor_enqueue_batch(const sle_job_motion_cmd_t *cmds, uint8_t count);
void sle_job_motion_executor_flush(void);
void sle_job_motion_executor_execute(const sle_job_motion_cmd_t *cmd);
void sle_job_motion_executor_set_origin(void);
bool sle_job_motion_executor_request_hold(uint32_t timeout_ms);
bool sle_job_motion_executor_resume(void);
bool sle_job_motion_executor_is_held(void);
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
uint32_t sle_job_motion_executor_completed_line(void);
unsigned long sle_job_motion_executor_last_activity_ms(void);
unsigned long sle_job_motion_executor_late_sample_count(void);
unsigned long sle_job_motion_executor_missed_sample_count(void);
unsigned long sle_job_motion_executor_motion_segment_count(void);
unsigned long sle_job_motion_executor_short_segment_count(void);
unsigned long sle_job_motion_executor_max_sample_late_us(void);
unsigned long sle_job_motion_executor_dac_write_count(void);
unsigned long sle_job_motion_executor_dac_skip_count(void);
void sle_job_motion_executor_get_diag(sle_job_motion_diag_t *diag);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_MOTION_EXECUTOR_H */
