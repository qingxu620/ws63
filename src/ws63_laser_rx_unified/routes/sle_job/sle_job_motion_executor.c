/**
 * @file motion_executor.c
 * @brief Local synchronous motion executor.
 */
#include "sle_job_motion_executor.h"
#include "sle_job_config.h"
#include "dac8563.h"
#include "laser_ctrl.h"
#include "soc_osal.h"
#include "timer.h"
#include "chip_core_irq.h"
#include "systick.h"
#include "tcxo.h"
#include "watchdog.h"
#include <math.h>
#include <string.h>

static double g_current_x = 0.0;
static double g_current_y = 0.0;
static volatile bool g_command_active = false;
static volatile bool g_motion_active = false;
static volatile bool g_abort_requested = false;
static volatile bool g_hold_requested = false;
static volatile bool g_motion_held = false;
static uint32_t g_hold_generation = 0;
static volatile uint32_t g_held_generation = 0;
static unsigned long g_last_activity_ms = 0;
static sle_job_motion_cmd_t g_motion_queue[SLE_JOB_MOTION_QUEUE_SIZE];
static volatile uint16_t g_queue_head = 0;
static volatile uint16_t g_queue_tail = 0;
static osal_mutex g_queue_mutex;
static osal_semaphore g_queue_sem;
static bool g_queue_ready = false;
static osal_semaphore g_hold_ack_sem;
static osal_semaphore g_resume_sem;
static bool g_hold_sems_ready = false;
static volatile bool g_worker_started = false;
static bool g_output_armed = false;
static volatile unsigned long g_enqueued_count = 0;
static volatile unsigned long g_executed_count = 0;
static volatile uint32_t g_completed_line = 0;
static volatile unsigned long g_late_sample_count = 0;
static volatile unsigned long g_missed_sample_count = 0;
static volatile unsigned long g_motion_segment_count = 0;
static volatile unsigned long g_short_segment_count = 0;
static volatile unsigned long g_max_sample_late_us = 0;
static volatile unsigned long g_arm_count = 0;
static volatile unsigned long g_move_count = 0;
static volatile unsigned long g_dac_write_count = 0;
static volatile unsigned long g_dac_skip_count = 0;
static volatile unsigned long g_wait_call_count = 0;
static volatile unsigned long g_sched_relief_count = 0;
static uint32_t g_sched_relief_samples = 0;
static volatile unsigned long g_timer_wait_count = 0;
static volatile unsigned long g_timer_fail_count = 0;
static volatile unsigned long g_timer_callback_count = 0;
static volatile unsigned long g_timer_callback_missing_count = 0;
static volatile uint32_t g_timer_wait_max_us = 0;
static volatile uint32_t g_timer_wake_late_max_us = 0;
static volatile unsigned long g_short_clamped_count = 0;
static volatile unsigned long g_deadline_catchup_count = 0;
static volatile uint64_t g_planned_motion_us = 0;
static volatile uint64_t g_actual_motion_us = 0;
static volatile uint64_t g_short_clamped_added_us = 0;
static volatile uint64_t g_deadline_late_total_us = 0;
static volatile uint32_t g_late_histogram[6] = {0};
static volatile uint64_t g_timer_start_total_us = 0;
static volatile uint64_t g_timer_block_total_us = 0;
static volatile uint64_t g_timer_wake_late_total_us = 0;
static volatile uint32_t g_timer_wake_late_histogram[6] = {0};
static volatile unsigned long g_deadline_reset_count = 0;
static volatile uint64_t g_deadline_reset_discarded_us = 0;
static volatile uint16_t g_queue_min_depth = UINT16_MAX;
static volatile uint16_t g_queue_max_depth = 0;
static volatile uint64_t g_queue_depth_sum = 0;
static volatile uint32_t g_queue_depth_samples = 0;
static volatile unsigned long g_queue_empty_count = 0;
static timer_handle_t g_motion_timer = NULL;
static osal_semaphore g_motion_timer_sem;
static bool g_motion_timer_sem_ready = false;
static volatile bool g_motion_timer_waiting = false;
static bool g_motion_timer_ready = false;
static volatile bool g_motion_timer_fault = false;
static uint32_t g_motion_timer_generation = 0;
static volatile uint32_t g_motion_timer_active_generation = 0;
static volatile uint32_t g_motion_timer_callback_generation = 0;
typedef struct {
    bool valid;
    uint64_t timer_target_us;
    uint64_t wake_us;
    uint64_t block_elapsed_us;
} motion_timer_stats_pending_t;
static motion_timer_stats_pending_t g_motion_timer_stats_pending;
static volatile uint64_t g_dac_total_us = 0;
static volatile uint64_t g_wait_total_us = 0;
static volatile uint32_t g_dac_max_us = 0;
static volatile uint32_t g_wait_max_us = 0;
static volatile uint32_t g_min_planned_step_us = 0;
static volatile uint32_t g_max_planned_step_us = 0;
static volatile uint32_t g_min_dac_gap_us = 0;
static volatile uint32_t g_max_dac_gap_us = 0;
static uint64_t g_last_dac_start_us = 0;
static uint16_t g_last_dac_x = 0;
static uint16_t g_last_dac_y = 0;
static bool g_last_dac_valid = false;
static uint64_t g_g0_settle_until_us = 0;
static uint64_t g_last_wdt_kick_us = 0;
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
static uint32_t g_last_move_slow_log_ms = 0;
#endif

#define SLE_JOB_MOTION_LATE_WARN_US 100U
typedef enum {
    MOTION_WAIT_REACHED = 0,
    MOTION_WAIT_HOLD,
    MOTION_WAIT_ABORT,
} motion_wait_result_t;

static uint16_t motion_queue_used_locked(void);

static inline uint32_t clamp_u64_to_u32(uint64_t value)
{
    return (value > UINT32_MAX) ? UINT32_MAX : (uint32_t)value;
}

static void record_latency_histogram(volatile uint32_t histogram[6], uint64_t latency_us)
{
    if (latency_us < 100U) {
        histogram[0]++;
    } else if (latency_us < 500U) {
        histogram[1]++;
    } else if (latency_us < 1000U) {
        histogram[2]++;
    } else if (latency_us < 3000U) {
        histogram[3]++;
    } else if (latency_us < 10000U) {
        histogram[4]++;
    } else {
        histogram[5]++;
    }
}

/* Run only after the time-critical action following a timer wake has completed. */
static void process_pending_timer_stats(void)
{
    if (!g_motion_timer_stats_pending.valid) {
        return;
    }

    motion_timer_stats_pending_t sample = g_motion_timer_stats_pending;
    g_motion_timer_stats_pending.valid = false;
    g_timer_block_total_us += sample.block_elapsed_us;

    if (sample.wake_us > sample.timer_target_us) {
        uint64_t wake_late_us = sample.wake_us - sample.timer_target_us;
        g_timer_wake_late_total_us += wake_late_us;
        record_latency_histogram(g_timer_wake_late_histogram, wake_late_us);
        if (wake_late_us > g_timer_wake_late_max_us) {
            g_timer_wake_late_max_us = clamp_u64_to_u32(wake_late_us);
        }
    }
}

static inline double clamp_axis(double value, double min_value, double max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static inline uint16_t mm_to_dac(double mm, double scale)
{
    double value = mm * scale;
    if (value < 0.0) {
        value = 0.0;
    }
    if (value > (double)SLE_JOB_DAC_MAX) {
        value = (double)SLE_JOB_DAC_MAX;
    }
    return (uint16_t)(value + 0.5);
}

static bool write_position(double x_mm, double y_mm, bool force)
{
    uint16_t x = mm_to_dac(x_mm, SLE_JOB_BEILV_X);
    uint16_t y = mm_to_dac(y_mm, SLE_JOB_BEILV_Y);
    if (!force && g_last_dac_valid && x == g_last_dac_x && y == g_last_dac_y) {
        g_current_x = x_mm;
        g_current_y = y_mm;
        g_dac_skip_count++;
        return true;
    }

    uint64_t start_us = uapi_tcxo_get_us();
    if (g_last_dac_start_us != 0U) {
        uint64_t gap_us = start_us - g_last_dac_start_us;
        uint32_t gap = (gap_us > UINT32_MAX) ? UINT32_MAX : (uint32_t)gap_us;
        if (g_min_dac_gap_us == 0U || gap < g_min_dac_gap_us) {
            g_min_dac_gap_us = gap;
        }
        if (gap > g_max_dac_gap_us) {
            g_max_dac_gap_us = gap;
        }
    }
    g_last_dac_start_us = start_us;
    errcode_t ret = dac8563_write_xy(x, y);
    uint64_t elapsed_us = uapi_tcxo_get_us() - start_us;
    g_dac_total_us += elapsed_us;
    if (elapsed_us > g_dac_max_us) {
        g_dac_max_us = (elapsed_us > UINT32_MAX) ? UINT32_MAX : (uint32_t)elapsed_us;
    }
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_MOTION] DAC write failed ret=0x%x, abort motion\r\n", ret);
        laser_force_off();
        g_abort_requested = true;
        g_output_armed = false;
        g_last_dac_valid = false;
        return false;
    }
    g_last_dac_x = x;
    g_last_dac_y = y;
    g_last_dac_valid = true;
    g_current_x = x_mm;
    g_current_y = y_mm;
    g_dac_write_count++;
    return true;
}

static bool write_current_position(bool force)
{
    return write_position(g_current_x, g_current_y, force);
}

static bool arm_output_if_needed(void)
{
    if (g_output_armed) {
        return true;
    }

    uint32_t start_ms = (uint32_t)uapi_systick_get_ms();
    laser_force_off();
    uint32_t off_ms = (uint32_t)uapi_systick_get_ms() - start_ms;

#if SLE_JOB_DAC_RECOVER_ON_ARM
    uint32_t recover_start_ms = (uint32_t)uapi_systick_get_ms();
    errcode_t recover_ret = dac8563_recover();
    uint32_t recover_ms = (uint32_t)uapi_systick_get_ms() - recover_start_ms;
    if (recover_ret != ERRCODE_SUCC) {
        osal_printk("[JOB_MOTION] DAC recover failed ret=0x%x\r\n", recover_ret);
        laser_force_off();
        g_abort_requested = true;
        return false;
    }
#else
    uint32_t recover_ms = 0;
#endif

    uint32_t pos_start_ms = (uint32_t)uapi_systick_get_ms();
    if (!write_current_position(true)) {
        return false;
    }
    uint32_t pos_ms = (uint32_t)uapi_systick_get_ms() - pos_start_ms;
    g_output_armed = true;
    g_arm_count++;

    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - start_ms;
    if (total_ms >= SLE_JOB_TIMING_SLOW_MS) {
        osal_printk("[JOB_MOTION_ARM] count=%lu recover=%u off_ms=%u recover_ms=%u "
                    "pos_ms=%u total_ms=%u x_um=%d y_um=%d\r\n",
                    g_arm_count,
                    (unsigned int)SLE_JOB_DAC_RECOVER_ON_ARM,
                    (unsigned int)off_ms,
                    (unsigned int)recover_ms,
                    (unsigned int)pos_ms,
                    (unsigned int)total_ms,
                    (int)(g_current_x * 1000.0),
                    (int)(g_current_y * 1000.0));
    }
    return true;
}

static void update_activity(void)
{
    g_last_activity_ms = (unsigned long)uapi_systick_get_ms();
}

static void kick_watchdog_periodic(uint64_t now_us, bool force)
{
    if (force || g_last_wdt_kick_us == 0 ||
        (now_us - g_last_wdt_kick_us) >= SLE_JOB_MOTION_WDT_KICK_INTERVAL_US) {
        (void)uapi_watchdog_kick();
        g_last_wdt_kick_us = now_us;
    }
}

static bool motion_wait_while_held(uint64_t *held_us)
{
    if (!g_hold_requested) {
        return !g_abort_requested && !g_motion_timer_fault;
    }
    if (!g_hold_sems_ready) {
        laser_force_off();
        g_abort_requested = true;
        return false;
    }

    uint64_t hold_start_us = uapi_tcxo_get_us();
    laser_force_off();
    /* An intentional pause must not appear as a pathological DAC scheduling gap. */
    g_last_dac_start_us = 0U;

    bool released = false;
    while (true) {
        uint32_t irq_state = osal_irq_lock();
        if (g_abort_requested || g_motion_timer_fault) {
            g_motion_held = false;
            osal_irq_restore(irq_state);
            break;
        }
        if (!g_hold_requested) {
            /* Check and clear atomically so a rapid RESUME -> STOP cannot slip
             * between observing release and clearing the physical-held state. */
            g_motion_held = false;
            released = true;
            osal_irq_restore(irq_state);
            break;
        }

        uint32_t generation = g_hold_generation;
        bool signal_ack = !g_motion_held || g_held_generation != generation;
        g_motion_held = true;
        g_held_generation = generation;
        osal_irq_restore(irq_state);

        if (signal_ack) {
            /* RESUME followed immediately by STOP may replace the hold generation
             * before this task gets CPU time. Remain stopped and ACK the new hold. */
            osal_sem_up(&g_hold_ack_sem);
        }
        (void)osal_sem_down(&g_resume_sem);
    }

    if (held_us != NULL) {
        *held_us += uapi_tcxo_get_us() - hold_start_us;
    }
    return released;
}

static bool motion_prepare_laser(bool laser_marking, uint16_t laser_power,
                                 uint64_t *held_us)
{
    while (!g_abort_requested && !g_motion_timer_fault) {
        if (g_hold_requested && !motion_wait_while_held(held_us)) {
            return false;
        }
        if (!laser_marking) {
            laser_enable(false);
            if (g_hold_requested) {
                continue;
            }
            return true;
        }

        laser_set_power(laser_power);
        if (g_hold_requested) {
            laser_force_off();
            continue;
        }
        laser_enable(true);
        if (g_hold_requested) {
            laser_force_off();
            continue;
        }
        return true;
    }
    laser_force_off();
    return false;
}

static void record_wait_duration(uint64_t start_us, uint64_t end_us)
{
    uint64_t elapsed_us = end_us - start_us;
    g_wait_call_count++;
    g_wait_total_us += elapsed_us;
    if (elapsed_us > g_wait_max_us) {
        g_wait_max_us = (elapsed_us > UINT32_MAX) ? UINT32_MAX : (uint32_t)elapsed_us;
    }
}

static void motion_timer_callback(uintptr_t data)
{
    uint32_t generation = (uint32_t)data;
    if (!g_motion_timer_waiting || generation == 0U ||
        generation != g_motion_timer_active_generation) {
        return;
    }

    g_motion_timer_callback_generation = generation;
    g_motion_timer_waiting = false;
    osal_sem_up(&g_motion_timer_sem);
    g_timer_callback_count++;
}

static bool wait_with_motion_timer(uint64_t target_us, uint64_t now_us)
{
    uint64_t remain_us = target_us - now_us;
    if (!g_motion_timer_ready || remain_us <= SLE_JOB_MOTION_TIMER_THRESHOLD_US) {
        return false;
    }

    uint64_t timer_target_us = target_us - SLE_JOB_MOTION_TIMER_TAIL_US;
    uint32_t timer_us = (uint32_t)(remain_us - SLE_JOB_MOTION_TIMER_TAIL_US);
    uint32_t generation;
    uint64_t start_begin_us = uapi_tcxo_get_us();
    uint32_t irq_state = osal_irq_lock();
    if (g_abort_requested || g_hold_requested) {
        osal_irq_restore(irq_state);
        return true;
    }
    generation = g_motion_timer_generation + 1U;
    if (generation == 0U) {
        generation = 1U;
    }
    g_motion_timer_generation = generation;
    g_motion_timer_callback_generation = 0U;
    g_motion_timer_active_generation = generation;
    g_motion_timer_waiting = true;
    errcode_t ret = uapi_timer_start(g_motion_timer, timer_us, motion_timer_callback,
                                     (uintptr_t)generation);
    if (ret != ERRCODE_SUCC) {
        g_motion_timer_waiting = false;
        g_motion_timer_active_generation = 0U;
    }
    osal_irq_restore(irq_state);
    g_timer_start_total_us += uapi_tcxo_get_us() - start_begin_us;
    if (ret != ERRCODE_SUCC) {
        g_timer_fail_count++;
        g_motion_timer_fault = true;
        osal_printk("[JOB_MOTION_TIMER_FAIL] op=start ret=0x%x wait_us=%u\r\n",
                    (unsigned int)ret, (unsigned int)timer_us);
        laser_force_off();
        g_abort_requested = true;
        return true;
    }

    g_timer_wait_count++;
    if (timer_us > g_timer_wait_max_us) {
        g_timer_wait_max_us = timer_us;
    }
    uint32_t timeout_ms = (timer_us + 999U) / 1000U + SLE_JOB_MOTION_TIMER_TIMEOUT_MARGIN_MS;
    uint64_t block_begin_us = uapi_tcxo_get_us();
    int sem_ret = osal_sem_down_timeout(&g_motion_timer_sem, timeout_ms);
    uint64_t wake_us = uapi_tcxo_get_us();
    if (sem_ret != OSAL_SUCCESS) {
        g_timer_block_total_us += wake_us - block_begin_us;
        irq_state = osal_irq_lock();
        g_motion_timer_waiting = false;
        g_motion_timer_active_generation = 0U;
        osal_irq_restore(irq_state);
        (void)uapi_timer_stop(g_motion_timer);
        if (g_motion_timer_callback_generation != generation) {
            g_timer_callback_missing_count++;
        }
        g_timer_fail_count++;
        g_motion_timer_fault = true;
        osal_printk("[JOB_MOTION_TIMER_FAIL] op=wait wait_us=%u timeout_ms=%u\r\n",
                    (unsigned int)timer_us, (unsigned int)timeout_ms);
        laser_force_off();
        g_abort_requested = true;
        return true;
    }

    uint32_t callback_generation = g_motion_timer_callback_generation;
    g_motion_timer_waiting = false;
    g_motion_timer_active_generation = 0U;
    bool callback_valid = callback_generation == generation;
    if (!callback_valid && !g_abort_requested && !g_hold_requested) {
        g_timer_callback_missing_count++;
    }
    if (callback_valid) {
        g_motion_timer_stats_pending = (motion_timer_stats_pending_t) {
            .valid = true,
            .timer_target_us = timer_target_us,
            .wake_us = wake_us,
            .block_elapsed_us = wake_us - block_begin_us,
        };
    } else {
        g_timer_block_total_us += wake_us - block_begin_us;
    }
    return true;
}

static motion_wait_result_t delay_until_us_interruptible(uint64_t target_us)
{
    uint64_t start_us = uapi_tcxo_get_us();
    uint64_t now_us = start_us;
    while (!g_abort_requested && !g_hold_requested && !g_motion_timer_fault) {
        kick_watchdog_periodic(now_us, false);
        if (now_us >= target_us) {
            record_wait_duration(start_us, now_us);
            return MOTION_WAIT_REACHED;
        }

        uint64_t remain_us = target_us - now_us;
        if (wait_with_motion_timer(target_us, now_us)) {
            now_us = uapi_tcxo_get_us();
            continue;
        }
        uint32_t chunk_us = (remain_us > SLE_JOB_MOTION_DELAY_CHUNK_US) ?
                            SLE_JOB_MOTION_DELAY_CHUNK_US : (uint32_t)remain_us;
        if (g_abort_requested || g_hold_requested || g_motion_timer_fault) {
            break;
        }
        if (chunk_us > 0U) {
            uapi_tcxo_delay_us(chunk_us);
        }
        now_us = uapi_tcxo_get_us();
    }

    record_wait_duration(start_us, uapi_tcxo_get_us());
    return g_hold_requested ? MOTION_WAIT_HOLD : MOTION_WAIT_ABORT;
}

static bool wait_for_g0_settle_if_needed(void)
{
    while (!g_abort_requested && !g_motion_timer_fault) {
        uint64_t settle_until_us = g_g0_settle_until_us;
        if (settle_until_us == 0U) {
            return true;
        }
        motion_wait_result_t wait_result = delay_until_us_interruptible(settle_until_us);
        process_pending_timer_stats();
        if (wait_result == MOTION_WAIT_REACHED) {
            g_g0_settle_until_us = 0;
            return true;
        }
        if (wait_result == MOTION_WAIT_HOLD) {
            if (!motion_wait_while_held(NULL)) {
                break;
            }
            g_g0_settle_until_us = uapi_tcxo_get_us() + SLE_JOB_G0_SETTLE_US;
            continue;
        }
        break;
    }
    laser_force_off();
    return false;
}

static int ceil_step_count(double value)
{
    if (value <= 1.0) {
        return 1;
    }
    return (int)ceil(value);
}

static void record_late_sample(uint64_t late_us)
{
    g_deadline_late_total_us += late_us;
    if (late_us < 100U) {
        g_late_histogram[0]++;
    } else if (late_us < 500U) {
        g_late_histogram[1]++;
    } else if (late_us < 1000U) {
        g_late_histogram[2]++;
    } else if (late_us < 3000U) {
        g_late_histogram[3]++;
    } else if (late_us < 10000U) {
        g_late_histogram[4]++;
    } else {
        g_late_histogram[5]++;
    }
    if (late_us > (uint64_t)g_max_sample_late_us) {
        g_max_sample_late_us = (unsigned long)late_us;
    }
    if (late_us >= SLE_JOB_MOTION_LATE_WARN_US) {
        g_late_sample_count++;
    }
}

static bool perform_move(double target_x, double target_y, double feed_rate_mm_min,
                         double spatial_step_mm, bool laser_marking, uint16_t laser_power)
{
    uint64_t actual_start_us = uapi_tcxo_get_us();
    uint64_t held_total_us = 0;
    bool move_completed = false;
    double distance = 0.0;
    double duration_us = 0.0;
    int steps = 0;
    double start_x;
    double start_y;
    double dx;
    double dy;
    double feed_sec;
    double unclamped_duration_us;
    bool short_segment;
    int time_steps;
    int space_steps;
    double step_time_us;
    uint32_t planned_step_us;
    double next_step_us;
    uint32_t catchup_budget;
    g_motion_active = true;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);

    target_x = clamp_axis(target_x, SLE_JOB_GALVO_X_MIN_MM, SLE_JOB_GALVO_X_MAX_MM);
    target_y = clamp_axis(target_y, SLE_JOB_GALVO_Y_MIN_MM, SLE_JOB_GALVO_Y_MAX_MM);

    if (!motion_prepare_laser(laser_marking, laser_power, &held_total_us)) {
        goto move_done;
    }

    start_x = g_current_x;
    start_y = g_current_y;
    dx = target_x - start_x;
    dy = target_y - start_y;
    distance = sqrt(dx * dx + dy * dy);

    if (distance <= 0.000001) {
        if (g_hold_requested &&
            !motion_prepare_laser(laser_marking, laser_power, &held_total_us)) {
            goto move_done;
        }
        move_completed = write_position(target_x, target_y, false);
        goto move_done;
    }

    feed_sec = feed_rate_mm_min / 60.0;
    if (feed_sec < 0.1) {
        feed_sec = 0.1;
    }

    duration_us = (distance / feed_sec) * 1000000.0;
    unclamped_duration_us = duration_us;
    g_motion_segment_count++;
    short_segment = (distance <= spatial_step_mm ||
                     duration_us <= (double)SLE_JOB_MOTION_SAMPLE_PERIOD_US);
    if (short_segment) {
        g_short_segment_count++;
    }
    if (laser_marking && short_segment && duration_us < (double)SLE_JOB_MOTION_MIN_MARK_SEGMENT_US) {
        duration_us = (double)SLE_JOB_MOTION_MIN_MARK_SEGMENT_US;
        g_short_clamped_count++;
        g_short_clamped_added_us += (uint64_t)(duration_us - unclamped_duration_us + 0.5);
    }
    g_planned_motion_us += (uint64_t)(duration_us + 0.5);

    time_steps = ceil_step_count(duration_us / (double)SLE_JOB_MOTION_SAMPLE_PERIOD_US);
    space_steps = ceil_step_count(distance / spatial_step_mm);
    steps = (time_steps > space_steps) ? time_steps : space_steps;
    step_time_us = duration_us / (double)steps;
    if (step_time_us < 1.0) {
        step_time_us = 1.0;
    }
    planned_step_us = (step_time_us >= (double)UINT32_MAX) ?
                      UINT32_MAX : (uint32_t)(step_time_us + 0.5);
    if (g_min_planned_step_us == 0U || planned_step_us < g_min_planned_step_us) {
        g_min_planned_step_us = planned_step_us;
    }
    if (planned_step_us > g_max_planned_step_us) {
        g_max_planned_step_us = planned_step_us;
    }

    next_step_us = (double)uapi_tcxo_get_us();
    catchup_budget = SLE_JOB_MOTION_CATCHUP_MAX_STEPS;
    for (int i = 1; i <= steps; i++) {
        bool sample_written = false;
        while (!sample_written && !g_abort_requested && !g_motion_timer_fault) {
            if (g_hold_requested) {
                if (!motion_prepare_laser(laser_marking, laser_power, &held_total_us)) {
                    goto move_done;
                }
                /* Issue the first remaining sample immediately after re-arming.
                 * Waiting a full step here would burn at the stationary pause point. */
                next_step_us = (double)uapi_tcxo_get_us() - step_time_us;
                catchup_budget = SLE_JOB_MOTION_CATCHUP_MAX_STEPS;
            }

            next_step_us += step_time_us;
            uint64_t target_us = (uint64_t)(next_step_us + 0.5);
            motion_wait_result_t wait_result = delay_until_us_interruptible(target_us);
            if (wait_result != MOTION_WAIT_REACHED) {
                process_pending_timer_stats();
                if (wait_result == MOTION_WAIT_HOLD) {
                    continue;
                }
                goto move_done;
            }
            if (g_hold_requested) {
                continue;
            }

            uint64_t now_us = uapi_tcxo_get_us();
            if (now_us > target_us) {
                uint64_t late_us = now_us - target_us;
                record_late_sample(late_us);
                if (late_us >= SLE_JOB_MOTION_LATE_WARN_US) {
                    unsigned long slipped = (unsigned long)((double)late_us / step_time_us);
                    g_missed_sample_count += (slipped > 0UL) ? slipped : 1UL;
                    if (catchup_budget > 0U && slipped > 0UL) {
                        double reset_us = (double)now_us - step_time_us;
                        if (reset_us > next_step_us) {
                            g_deadline_reset_discarded_us +=
                                (uint64_t)(reset_us - next_step_us + 0.5);
                        }
                        next_step_us = reset_us;
                        catchup_budget--;
                        g_deadline_catchup_count++;
                    } else {
                        g_deadline_reset_discarded_us +=
                            (uint64_t)((double)now_us - next_step_us + 0.5);
                        next_step_us = (double)now_us;
                    }
                    g_deadline_reset_count++;
                }
            }

            double fraction = (double)i / (double)steps;
            double sample_x = start_x + dx * fraction;
            double sample_y = start_y + dy * fraction;
            if (g_hold_requested) {
                continue;
            }
            sample_written = write_position(sample_x, sample_y, false);
            process_pending_timer_stats();
            if (!sample_written) {
                goto move_done;
            }

#if SLE_JOB_MOTION_SCHED_RELIEF_INTERVAL > 0U && SLE_JOB_MOTION_SCHED_RELIEF_MS > 0U
            g_sched_relief_samples++;
            if (g_sched_relief_samples >= SLE_JOB_MOTION_SCHED_RELIEF_INTERVAL) {
                g_sched_relief_samples = 0;
                g_sched_relief_count++;
                osal_msleep(SLE_JOB_MOTION_SCHED_RELIEF_MS);
            }
#endif

            kick_watchdog_periodic(now_us, false);
            if ((i % 200) == 0) {
                update_activity();
            }
        }
        if (!sample_written) {
            goto move_done;
        }
    }

    if (!g_abort_requested && !g_motion_timer_fault) {
        if (g_hold_requested &&
            !motion_prepare_laser(laser_marking, laser_power, &held_total_us)) {
            goto move_done;
        }
        move_completed = write_position(target_x, target_y, false);
    }

move_done:
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    g_motion_active = false;
    uint64_t actual_elapsed_us = uapi_tcxo_get_us() - actual_start_us;
    if (actual_elapsed_us >= held_total_us) {
        actual_elapsed_us -= held_total_us;
    }
    g_actual_motion_us += actual_elapsed_us;

    g_move_count++;
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
    uint32_t wall_ms = (uint32_t)(actual_elapsed_us / 1000U);
    if (wall_ms >= SLE_JOB_MOTION_MOVE_SLOW_MS) {
        uint32_t now_ms = (uint32_t)uapi_systick_get_ms();
        if (g_last_move_slow_log_ms == 0U ||
            (uint32_t)(now_ms - g_last_move_slow_log_ms) >= SLE_JOB_MOTION_MOVE_SLOW_LOG_PERIOD_MS) {
            g_last_move_slow_log_ms = now_ms;
            osal_printk("[JOB_MOTION_MOVE_SLOW] move=%lu laser=%u dist_um=%u steps=%d "
                        "plan_ms=%u wall_ms=%u late=%lu missed=%lu max_late_us=%lu q=%u\r\n",
                        g_move_count,
                        (unsigned int)(laser_marking ? 1U : 0U),
                        (unsigned int)(distance * 1000.0 + 0.5),
                        steps,
                        (unsigned int)(duration_us / 1000.0 + 0.5),
                        (unsigned int)wall_ms,
                        g_late_sample_count,
                        g_missed_sample_count,
                        g_max_sample_late_us,
                        (unsigned int)sle_job_motion_executor_queue_depth());
        }
    }
#endif
    return move_completed && !g_abort_requested && !g_motion_timer_fault;
}

void sle_job_motion_executor_reset_stats(void)
{
    g_enqueued_count = 0;
    g_executed_count = 0;
    g_completed_line = 0;
    g_late_sample_count = 0;
    g_missed_sample_count = 0;
    g_motion_segment_count = 0;
    g_short_segment_count = 0;
    g_max_sample_late_us = 0;
    g_arm_count = 0;
    g_move_count = 0;
    g_dac_write_count = 0;
    g_dac_skip_count = 0;
    g_wait_call_count = 0;
    g_sched_relief_count = 0;
    g_sched_relief_samples = 0;
    g_timer_wait_count = 0;
    g_timer_fail_count = 0;
    g_timer_callback_count = 0;
    g_timer_callback_missing_count = 0;
    g_timer_wait_max_us = 0;
    g_timer_wake_late_max_us = 0;
    g_short_clamped_count = 0;
    g_deadline_catchup_count = 0;
    g_planned_motion_us = 0;
    g_actual_motion_us = 0;
    g_short_clamped_added_us = 0;
    g_deadline_late_total_us = 0;
    g_timer_start_total_us = 0;
    g_timer_block_total_us = 0;
    g_timer_wake_late_total_us = 0;
    g_deadline_reset_count = 0;
    g_deadline_reset_discarded_us = 0;
    memset((void *)g_late_histogram, 0, sizeof(g_late_histogram));
    memset((void *)g_timer_wake_late_histogram, 0, sizeof(g_timer_wake_late_histogram));
    memset(&g_motion_timer_stats_pending, 0, sizeof(g_motion_timer_stats_pending));
    g_queue_min_depth = UINT16_MAX;
    g_queue_max_depth = 0;
    g_queue_depth_sum = 0;
    g_queue_depth_samples = 0;
    g_queue_empty_count = 0;
    g_dac_total_us = 0;
    g_wait_total_us = 0;
    g_dac_max_us = 0;
    g_wait_max_us = 0;
    g_min_planned_step_us = 0;
    g_max_planned_step_us = 0;
    g_min_dac_gap_us = 0;
    g_max_dac_gap_us = 0;
    g_last_dac_start_us = 0;
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
    g_last_move_slow_log_ms = 0;
#endif
}

void sle_job_motion_executor_init(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_command_active = false;
    g_motion_active = false;
    g_abort_requested = false;
    g_hold_requested = false;
    g_motion_held = false;
    g_hold_generation = 0U;
    g_held_generation = 0U;
    g_last_activity_ms = 0;
    g_queue_head = 0;
    g_queue_tail = 0;
    g_output_armed = false;
    g_motion_timer_waiting = false;
    g_motion_timer_active_generation = 0U;
    g_motion_timer_callback_generation = 0U;
    g_motion_timer_fault = false;
    sle_job_motion_executor_reset_stats();
    g_last_dac_x = 0;
    g_last_dac_y = 0;
    g_last_dac_valid = false;
    g_g0_settle_until_us = 0;
    g_last_wdt_kick_us = 0;
    memset(g_motion_queue, 0, sizeof(g_motion_queue));
    if (!g_queue_ready) {
        if (osal_mutex_init(&g_queue_mutex) == OSAL_SUCCESS &&
            osal_sem_init(&g_queue_sem, 0) == OSAL_SUCCESS) {
            g_queue_ready = true;
        }
    }
    if (!g_hold_sems_ready &&
        osal_sem_init(&g_hold_ack_sem, 0) == OSAL_SUCCESS &&
        osal_sem_init(&g_resume_sem, 0) == OSAL_SUCCESS) {
        g_hold_sems_ready = true;
    }
    if (!g_hold_sems_ready) {
        osal_printk("[JOB_MOTION_HOLD] semaphore init failed\r\n");
    }
    while (g_hold_sems_ready &&
           osal_sem_down_timeout(&g_hold_ack_sem, 0) == OSAL_SUCCESS) {
    }
    while (g_hold_sems_ready &&
           osal_sem_down_timeout(&g_resume_sem, 0) == OSAL_SUCCESS) {
    }
    while (g_queue_ready &&
           osal_sem_down_timeout(&g_queue_sem, 0) == OSAL_SUCCESS) {
    }
    if (!g_motion_timer_sem_ready &&
        osal_sem_init(&g_motion_timer_sem, 0) == OSAL_SUCCESS) {
        g_motion_timer_sem_ready = true;
    }
    if (g_motion_timer_sem_ready && !g_motion_timer_ready &&
        uapi_timer_adapter(SLE_JOB_MOTION_TIMER_INDEX, TIMER_2_IRQN,
                           SLE_JOB_MOTION_TIMER_IRQ_PRIORITY) == ERRCODE_SUCC &&
        uapi_timer_create(SLE_JOB_MOTION_TIMER_INDEX, &g_motion_timer) == ERRCODE_SUCC) {
        g_motion_timer_ready = true;
        osal_printk("[JOB_MOTION_TIMER] ready index=%u threshold_us=%u tail_us=%u\r\n",
                    (unsigned int)SLE_JOB_MOTION_TIMER_INDEX,
                    (unsigned int)SLE_JOB_MOTION_TIMER_THRESHOLD_US,
                    (unsigned int)SLE_JOB_MOTION_TIMER_TAIL_US);
    }
    if (!g_motion_timer_ready) {
        osal_printk("[JOB_MOTION_TIMER] init failed, using busy wait\r\n");
    }
    while (g_motion_timer_sem_ready &&
           osal_sem_down_timeout(&g_motion_timer_sem, 0) == OSAL_SUCCESS) {
    }
    (void)write_current_position(true);
}

static bool motion_queue_pop(sle_job_motion_cmd_t *cmd)
{
    if (!g_queue_ready || !g_worker_started || cmd == NULL) {
        return false;
    }
    if (osal_sem_down(&g_queue_sem) != OSAL_SUCCESS) {
        return false;
    }

    osal_mutex_lock(&g_queue_mutex);
    if (g_queue_head == g_queue_tail) {
        osal_mutex_unlock(&g_queue_mutex);
        return false;
    }

    memcpy(cmd, &g_motion_queue[g_queue_tail], sizeof(sle_job_motion_cmd_t));
    g_queue_tail = (uint16_t)((g_queue_tail + 1U) % SLE_JOB_MOTION_QUEUE_SIZE);
    uint16_t depth = motion_queue_used_locked();
    if (depth < g_queue_min_depth) {
        g_queue_min_depth = depth;
    }
    if (depth > g_queue_max_depth) {
        g_queue_max_depth = depth;
    }
    g_queue_depth_sum += depth;
    g_queue_depth_samples++;
    if (depth == 0U) {
        g_queue_empty_count++;
    }
    g_command_active = true;
    osal_mutex_unlock(&g_queue_mutex);
    return true;
}

static uint16_t motion_queue_used_locked(void)
{
    if (g_queue_head >= g_queue_tail) {
        return (uint16_t)(g_queue_head - g_queue_tail);
    }
    return (uint16_t)(SLE_JOB_MOTION_QUEUE_SIZE - g_queue_tail + g_queue_head);
}

static bool cmd_uses_laser(const sle_job_motion_cmd_t *cmd)
{
    return cmd != NULL && cmd->cmd == SLE_JOB_CMD_G1_MOVE && ((cmd->flags & SLE_JOB_FLAG_LASER_ON) != 0) && cmd->laser_pwr > 0;
}

bool sle_job_motion_executor_enqueue(const sle_job_motion_cmd_t *cmd)
{
    return sle_job_motion_executor_enqueue_batch(cmd, 1U);
}

bool sle_job_motion_executor_enqueue_batch(const sle_job_motion_cmd_t *cmds, uint8_t count)
{
    if (!g_queue_ready || !g_worker_started || cmds == NULL || count == 0U ||
        g_motion_timer_fault) {
        return false;
    }

    while (!g_abort_requested && !g_motion_timer_fault) {
        osal_mutex_lock(&g_queue_mutex);
        uint16_t used = motion_queue_used_locked();
        uint16_t free_slots = (uint16_t)(SLE_JOB_MOTION_QUEUE_SIZE - 1U - used);
        if (free_slots >= count && !g_abort_requested && !g_motion_timer_fault) {
            for (uint8_t i = 0; i < count; i++) {
                memcpy(&g_motion_queue[g_queue_head], &cmds[i], sizeof(sle_job_motion_cmd_t));
                g_queue_head = (uint16_t)((g_queue_head + 1U) % SLE_JOB_MOTION_QUEUE_SIZE);
                uint16_t depth = motion_queue_used_locked();
                if (depth < g_queue_min_depth) {
                    g_queue_min_depth = depth;
                }
                if (depth > g_queue_max_depth) {
                    g_queue_max_depth = depth;
                }
                g_queue_depth_sum += depth;
                g_queue_depth_samples++;
            }
            g_enqueued_count += count;
            for (uint8_t i = 0; i < count; i++) {
                osal_sem_up(&g_queue_sem);
            }
            osal_mutex_unlock(&g_queue_mutex);
            return true;
        }
        osal_mutex_unlock(&g_queue_mutex);
        osal_msleep(1);
    }

    return false;
}

void sle_job_motion_executor_flush(void)
{
    if (!g_queue_ready) {
        return;
    }

    osal_mutex_lock(&g_queue_mutex);
    g_queue_head = 0;
    g_queue_tail = 0;
    while (osal_sem_down_timeout(&g_queue_sem, 0) == OSAL_SUCCESS) {
    }
    osal_mutex_unlock(&g_queue_mutex);
    laser_force_off();
    g_g0_settle_until_us = 0;
}

static int sle_job_motion_executor_task(void *arg)
{
    (void)arg;

    sle_job_motion_cmd_t cmd;
    while (1) {
        if (g_hold_requested && !motion_wait_while_held(NULL)) {
            osal_msleep(1);
            continue;
        }
        if (motion_queue_pop(&cmd)) {
            sle_job_motion_executor_execute(&cmd);
            g_command_active = false;
        } else {
            osal_msleep(1);
        }
    }

    return 0;
}

errcode_t sle_job_motion_executor_start_task(void)
{
    if (!g_queue_ready) {
        return ERRCODE_FAIL;
    }
    if (g_worker_started) {
        return ERRCODE_SUCC;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(sle_job_motion_executor_task, NULL, "laser_motion", SLE_JOB_TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_TASK_PRIO_MOTION) != OSAL_SUCCESS) {
        osal_printk("[laser single] set motion priority failed\r\n");
    }
    osal_kfree(task);
    g_worker_started = true;
    osal_kthread_unlock();
    return ERRCODE_SUCC;
}

void sle_job_motion_executor_execute(const sle_job_motion_cmd_t *cmd)
{
    if (cmd == NULL || g_motion_timer_fault) {
        laser_force_off();
        return;
    }
    if (g_hold_requested && !motion_wait_while_held(NULL)) {
        laser_force_off();
        return;
    }

    bool command_completed = false;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    switch (cmd->cmd) {
        case SLE_JOB_CMD_G0_MOVE:
            if (!arm_output_if_needed()) {
                break;
            }
            command_completed = perform_move(cmd->target_x, cmd->target_y,
                                             SLE_JOB_G0_FEED_RATE,
                                             SLE_JOB_G0_STEP_NUM, false, 0U);
            if (command_completed) {
                g_g0_settle_until_us = uapi_tcxo_get_us() + SLE_JOB_G0_SETTLE_US;
            }
            break;
        case SLE_JOB_CMD_G1_MOVE: {
            double feed_rate = cmd->feed_rate;
            bool laser_on = cmd_uses_laser(cmd);
            if (!arm_output_if_needed()) {
                break;
            }
            if (laser_on && feed_rate > SLE_JOB_MARKING_FEED_RATE_MAX) {
                feed_rate = SLE_JOB_MARKING_FEED_RATE_MAX;
            }
            if (laser_on) {
                if (!wait_for_g0_settle_if_needed()) {
                    break;
                }
            }
            command_completed = perform_move(cmd->target_x, cmd->target_y, feed_rate,
                                             SLE_JOB_STEP_NUM, laser_on, cmd->laser_pwr);
            if (!laser_on) {
                laser_enable(false);
            }
            break;
        }
        case SLE_JOB_CMD_LASER_ON:
            if (!wait_for_g0_settle_if_needed()) {
                break;
            }
            command_completed = motion_prepare_laser(cmd->laser_pwr > 0U,
                                                     cmd->laser_pwr, NULL);
            if (command_completed) {
                update_activity();
            }
            break;
        case SLE_JOB_CMD_LASER_OFF:
            laser_enable(false);
            laser_set_power(0);
            update_activity();
            command_completed = true;
            break;
        case SLE_JOB_CMD_SET_ORIGIN:
            laser_force_off();
            sle_job_motion_executor_set_origin();
            command_completed = !g_abort_requested && !g_motion_timer_fault;
            break;
        case SLE_JOB_CMD_EMERGENCY_STOP:
            sle_job_motion_executor_request_abort();
            sle_job_motion_executor_flush();
            laser_force_off();
            update_activity();
            break;
        case SLE_JOB_CMD_PROGRESS_MARK:
            update_activity();
            command_completed = true;
            break;
        default:
            break;
    }
    if (command_completed && !g_abort_requested && !g_motion_timer_fault &&
        cmd->completion_line > g_completed_line) {
        uint32_t expected_line = g_completed_line + 1U;
        if (cmd->completion_line != expected_line) {
            osal_printk("[JOB_MOTION_LINE_GAP] completed=%u next=%u expected=%u\r\n",
                        (unsigned int)g_completed_line,
                        (unsigned int)cmd->completion_line,
                        (unsigned int)expected_line);
            laser_force_off();
            sle_job_motion_executor_request_abort();
        } else {
            g_completed_line = cmd->completion_line;
        }
    }
    g_executed_count++;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void sle_job_motion_executor_set_origin(void)
{
    laser_force_off();
    (void)write_position(0.0, 0.0, true);
    g_g0_settle_until_us = g_abort_requested ? 0U :
                           uapi_tcxo_get_us() + SLE_JOB_G0_SETTLE_US;
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

bool sle_job_motion_executor_request_hold(uint32_t timeout_ms)
{
    if (!g_hold_sems_ready || !g_queue_ready || !g_worker_started) {
        return false;
    }

    bool wake_timer_wait = false;
    uint32_t irq_state = osal_irq_lock();
    if (g_abort_requested || g_motion_timer_fault) {
        osal_irq_restore(irq_state);
        return false;
    }
    if (!g_hold_requested) {
        g_hold_generation++;
        if (g_hold_generation == 0U) {
            g_hold_generation = 1U;
        }
        g_hold_requested = true;
    }
    uint32_t generation = g_hold_generation;
    if (g_motion_timer_ready && g_motion_timer_waiting) {
        g_motion_timer_waiting = false;
        g_motion_timer_active_generation = 0U;
        wake_timer_wait = true;
    }
    osal_irq_restore(irq_state);

    laser_force_off();
    if (wake_timer_wait) {
        (void)uapi_timer_stop(g_motion_timer);
        osal_sem_up(&g_motion_timer_sem);
    }
    /* Wake a worker which may be blocked on an empty motion queue. */
    osal_sem_up(&g_queue_sem);

    uint32_t start_ms = (uint32_t)uapi_systick_get_ms();
    while (true) {
        irq_state = osal_irq_lock();
        bool held = g_motion_held && g_held_generation == generation;
        bool failed = g_abort_requested || g_motion_timer_fault;
        osal_irq_restore(irq_state);
        if (held) {
            return true;
        }
        if (failed) {
            return false;
        }

        uint32_t elapsed_ms = (uint32_t)uapi_systick_get_ms() - start_ms;
        if (elapsed_ms >= timeout_ms) {
            return false;
        }
        uint32_t remaining_ms = timeout_ms - elapsed_ms;
        if (osal_sem_down_timeout(&g_hold_ack_sem, remaining_ms) != OSAL_SUCCESS) {
            return g_motion_held && g_held_generation == generation;
        }
    }
}

bool sle_job_motion_executor_resume(void)
{
    if (!g_hold_sems_ready) {
        return false;
    }

    uint32_t irq_state = osal_irq_lock();
    if (!g_hold_requested || !g_motion_held || g_abort_requested || g_motion_timer_fault) {
        osal_irq_restore(irq_state);
        return false;
    }
    g_hold_requested = false;
    osal_irq_restore(irq_state);
    osal_sem_up(&g_resume_sem);
    return true;
}

bool sle_job_motion_executor_is_held(void)
{
    return g_motion_held && g_hold_requested;
}

void sle_job_motion_executor_request_abort(void)
{
    bool wake_timer_wait = false;
    bool wake_hold_wait = false;
    uint32_t irq_state = osal_irq_lock();
    wake_hold_wait = g_hold_requested || g_motion_held;
    g_abort_requested = true;
    g_hold_requested = false;
    g_g0_settle_until_us = 0;
    if (g_motion_timer_ready && g_motion_timer_waiting) {
        g_motion_timer_waiting = false;
        g_motion_timer_active_generation = 0U;
        wake_timer_wait = true;
    }
    osal_irq_restore(irq_state);
    if (wake_timer_wait) {
        (void)uapi_timer_stop(g_motion_timer);
        osal_sem_up(&g_motion_timer_sem);
    }
    laser_force_off();
    if (g_hold_sems_ready && wake_hold_wait) {
        osal_sem_up(&g_resume_sem);
        osal_sem_up(&g_hold_ack_sem);
    }
    if (g_queue_ready) {
        osal_sem_up(&g_queue_sem);
    }
}

void sle_job_motion_executor_clear_abort(void)
{
    uint32_t irq_state = osal_irq_lock();
    g_motion_timer_waiting = false;
    g_motion_timer_active_generation = 0U;
    g_hold_requested = false;
    g_motion_held = false;
    g_held_generation = 0U;
    osal_irq_restore(irq_state);
    if (g_hold_sems_ready) {
        while (osal_sem_down_timeout(&g_hold_ack_sem, 0) == OSAL_SUCCESS) {
        }
        while (osal_sem_down_timeout(&g_resume_sem, 0) == OSAL_SUCCESS) {
        }
    }
    while (g_motion_timer_sem_ready &&
           osal_sem_down_timeout(&g_motion_timer_sem, 0) == OSAL_SUCCESS) {
    }
    memset(&g_motion_timer_stats_pending, 0, sizeof(g_motion_timer_stats_pending));
    g_motion_timer_fault = false;
    g_abort_requested = false;
}

double sle_job_motion_executor_get_x(void)
{
    return g_current_x;
}

double sle_job_motion_executor_get_y(void)
{
    return g_current_y;
}

bool sle_job_motion_executor_is_busy(void)
{
    if (g_hold_requested || g_motion_held) {
        return true;
    }
    if (g_command_active) {
        return true;
    }
    if (g_motion_active) {
        return true;
    }
    if (!g_queue_ready) {
        return false;
    }

    osal_mutex_lock(&g_queue_mutex);
    bool busy = (motion_queue_used_locked() > 0U);
    osal_mutex_unlock(&g_queue_mutex);
    return busy;
}

uint16_t sle_job_motion_executor_queue_depth(void)
{
    if (!g_queue_ready) {
        return 0;
    }

    osal_mutex_lock(&g_queue_mutex);
    uint16_t used = motion_queue_used_locked();
    osal_mutex_unlock(&g_queue_mutex);
    return used;
}

bool sle_job_motion_executor_worker_started(void)
{
    return g_worker_started;
}

bool sle_job_motion_executor_abort_requested(void)
{
    return g_abort_requested || g_motion_timer_fault;
}

unsigned long sle_job_motion_executor_enqueued_count(void)
{
    return g_enqueued_count;
}

unsigned long sle_job_motion_executor_executed_count(void)
{
    return g_executed_count;
}

uint32_t sle_job_motion_executor_completed_line(void)
{
    return g_completed_line;
}

unsigned long sle_job_motion_executor_last_activity_ms(void)
{
    return g_last_activity_ms;
}

unsigned long sle_job_motion_executor_late_sample_count(void)
{
    return g_late_sample_count;
}

unsigned long sle_job_motion_executor_missed_sample_count(void)
{
    return g_missed_sample_count;
}

unsigned long sle_job_motion_executor_motion_segment_count(void)
{
    return g_motion_segment_count;
}

unsigned long sle_job_motion_executor_short_segment_count(void)
{
    return g_short_segment_count;
}

unsigned long sle_job_motion_executor_max_sample_late_us(void)
{
    return g_max_sample_late_us;
}

unsigned long sle_job_motion_executor_dac_write_count(void)
{
    return g_dac_write_count;
}

unsigned long sle_job_motion_executor_dac_skip_count(void)
{
    return g_dac_skip_count;
}

void sle_job_motion_executor_get_diag(sle_job_motion_diag_t *diag)
{
    if (diag == NULL) {
        return;
    }

    uint32_t irq_state = osal_irq_lock();
    diag->dac_write_count = g_dac_write_count;
    diag->dac_skip_count = g_dac_skip_count;
    diag->wait_call_count = g_wait_call_count;
    diag->late_sample_count = g_late_sample_count;
    diag->missed_sample_count = g_missed_sample_count;
    diag->max_sample_late_us = g_max_sample_late_us;
    diag->sched_relief_count = g_sched_relief_count;
    diag->timer_wait_count = g_timer_wait_count;
    diag->timer_fail_count = g_timer_fail_count;
    diag->timer_callback_count = g_timer_callback_count;
    diag->timer_callback_missing_count = g_timer_callback_missing_count;
    diag->dac_total_us = g_dac_total_us;
    diag->wait_total_us = g_wait_total_us;
    diag->dac_max_us = g_dac_max_us;
    diag->wait_max_us = g_wait_max_us;
    diag->min_planned_step_us = g_min_planned_step_us;
    diag->max_planned_step_us = g_max_planned_step_us;
    diag->min_dac_gap_us = g_min_dac_gap_us;
    diag->max_dac_gap_us = g_max_dac_gap_us;
    diag->timer_wait_max_us = g_timer_wait_max_us;
    diag->timer_wake_late_max_us = g_timer_wake_late_max_us;
    diag->short_clamped_count = g_short_clamped_count;
    diag->deadline_catchup_count = g_deadline_catchup_count;
    diag->queue_empty_count = g_queue_empty_count;
    diag->planned_motion_us = g_planned_motion_us;
    diag->actual_motion_us = g_actual_motion_us;
    diag->short_clamped_added_us = g_short_clamped_added_us;
    diag->deadline_late_total_us = g_deadline_late_total_us;
    diag->timer_start_total_us = g_timer_start_total_us;
    diag->timer_block_total_us = g_timer_block_total_us;
    diag->timer_wake_late_total_us = g_timer_wake_late_total_us;
    diag->deadline_reset_count = g_deadline_reset_count;
    diag->deadline_reset_discarded_us = g_deadline_reset_discarded_us;
    memcpy(diag->late_histogram, (const void *)g_late_histogram, sizeof(diag->late_histogram));
    memcpy(diag->timer_wake_late_histogram, (const void *)g_timer_wake_late_histogram,
           sizeof(diag->timer_wake_late_histogram));
    diag->queue_min_depth = (g_queue_min_depth == UINT16_MAX) ? 0U : g_queue_min_depth;
    diag->queue_max_depth = g_queue_max_depth;
    diag->queue_avg_depth = (g_queue_depth_samples == 0U) ? 0U :
        (uint16_t)(g_queue_depth_sum / g_queue_depth_samples);
    osal_irq_restore(irq_state);
}
