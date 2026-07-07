/**
 * @file motion_executor.c
 * @brief Local synchronous motion executor.
 */
#include "sle_job_motion_executor.h"
#include "sle_job_config.h"
#include "dac8562.h"
#include "laser_ctrl.h"
#include "soc_osal.h"
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
static unsigned long g_last_activity_ms = 0;
static sle_job_motion_cmd_t g_motion_queue[SLE_JOB_MOTION_QUEUE_SIZE];
static volatile uint16_t g_queue_head = 0;
static volatile uint16_t g_queue_tail = 0;
static osal_mutex g_queue_mutex;
static osal_semaphore g_queue_sem;
static bool g_queue_ready = false;
static volatile bool g_worker_started = false;
static bool g_output_armed = false;
static volatile unsigned long g_enqueued_count = 0;
static volatile unsigned long g_executed_count = 0;
static volatile unsigned long g_late_sample_count = 0;
static volatile unsigned long g_missed_sample_count = 0;
static volatile unsigned long g_motion_segment_count = 0;
static volatile unsigned long g_short_segment_count = 0;
static volatile unsigned long g_max_sample_late_us = 0;
static volatile unsigned long g_arm_count = 0;
static volatile unsigned long g_move_count = 0;
static volatile unsigned long g_dac_write_count = 0;
static volatile unsigned long g_dac_skip_count = 0;
static uint16_t g_last_dac_x = 0;
static uint16_t g_last_dac_y = 0;
static bool g_last_dac_valid = false;
static uint64_t g_last_wdt_kick_us = 0;
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
static uint32_t g_last_move_slow_log_ms = 0;
#endif

#define SLE_JOB_MOTION_LATE_WARN_US 100U

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

static void write_current_position(bool force)
{
    uint16_t x = mm_to_dac(g_current_x, SLE_JOB_BEILV_X);
    uint16_t y = mm_to_dac(g_current_y, SLE_JOB_BEILV_Y);
    if (!force && g_last_dac_valid && x == g_last_dac_x && y == g_last_dac_y) {
        g_dac_skip_count++;
        return;
    }

    dac8562_write_xy(x, y);
    g_last_dac_x = x;
    g_last_dac_y = y;
    g_last_dac_valid = true;
    g_dac_write_count++;
}

static void arm_output_if_needed(void)
{
    if (g_output_armed) {
        return;
    }

    uint32_t start_ms = (uint32_t)uapi_systick_get_ms();
    laser_force_off();
    uint32_t off_ms = (uint32_t)uapi_systick_get_ms() - start_ms;

#if SLE_JOB_DAC_RECOVER_ON_ARM
    uint32_t recover_start_ms = (uint32_t)uapi_systick_get_ms();
    dac8562_recover();
    uint32_t recover_ms = (uint32_t)uapi_systick_get_ms() - recover_start_ms;
#else
    uint32_t recover_ms = 0;
#endif

    uint32_t pos_start_ms = (uint32_t)uapi_systick_get_ms();
    write_current_position(true);
    uint32_t pos_ms = (uint32_t)uapi_systick_get_ms() - pos_start_ms;
    g_output_armed = true;
    g_arm_count++;

    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - start_ms;
    if (total_ms >= SLE_JOB_TIMING_SLOW_MS || g_arm_count <= 4UL) {
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

static bool delay_until_us_interruptible(uint64_t target_us)
{
    while (!g_abort_requested) {
        uint64_t now_us = uapi_tcxo_get_us();
        kick_watchdog_periodic(now_us, false);
        if (now_us >= target_us) {
            return true;
        }

        uint64_t remain_us = target_us - now_us;
        if (remain_us >= SLE_JOB_MOTION_SLEEP_THRESHOLD_US) {
            osal_msleep(1);
            continue;
        }

        uint32_t chunk_us = (remain_us > SLE_JOB_MOTION_DELAY_CHUNK_US) ?
                            SLE_JOB_MOTION_DELAY_CHUNK_US : (uint32_t)remain_us;
        if (g_abort_requested) {
            return false;
        }
        if (chunk_us > 0U) {
            uapi_tcxo_delay_us(chunk_us);
            osal_yield();
        }
    }

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
    if (late_us > (uint64_t)g_max_sample_late_us) {
        g_max_sample_late_us = (unsigned long)late_us;
    }
    if (late_us >= SLE_JOB_MOTION_LATE_WARN_US) {
        g_late_sample_count++;
    }
}

static void perform_move(double target_x, double target_y, double feed_rate_mm_min, bool laser_marking)
{
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
    uint32_t move_start_ms = (uint32_t)uapi_systick_get_ms();
#endif
    g_motion_active = true;
    g_abort_requested = false;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);

    target_x = clamp_axis(target_x, SLE_JOB_GALVO_X_MIN_MM, SLE_JOB_GALVO_X_MAX_MM);
    target_y = clamp_axis(target_y, SLE_JOB_GALVO_Y_MIN_MM, SLE_JOB_GALVO_Y_MAX_MM);

    double start_x = g_current_x;
    double start_y = g_current_y;
    double dx = target_x - start_x;
    double dy = target_y - start_y;
    double distance = sqrt(dx * dx + dy * dy);

    if (distance <= 0.000001) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position(false);
        update_activity();
        kick_watchdog_periodic(uapi_tcxo_get_us(), true);
        g_motion_active = false;
        return;
    }

    double feed_sec = feed_rate_mm_min / 60.0;
    if (feed_sec < 0.1) {
        feed_sec = 0.1;
    }

    double duration_us = (distance / feed_sec) * 1000000.0;
    g_motion_segment_count++;
    bool short_segment = (distance <= SLE_JOB_STEP_NUM || duration_us <= (double)SLE_JOB_MOTION_SAMPLE_PERIOD_US);
    if (short_segment) {
        g_short_segment_count++;
    }
    if (laser_marking && short_segment && duration_us < (double)SLE_JOB_MOTION_MIN_MARK_SEGMENT_US) {
        duration_us = (double)SLE_JOB_MOTION_MIN_MARK_SEGMENT_US;
    }

    int time_steps = ceil_step_count(duration_us / (double)SLE_JOB_MOTION_SAMPLE_PERIOD_US);
    int space_steps = ceil_step_count(distance / SLE_JOB_STEP_NUM);
    int steps = (time_steps > space_steps) ? time_steps : space_steps;
    double step_time_us = duration_us / (double)steps;
    if (step_time_us < 1.0) {
        step_time_us = 1.0;
    }

    double next_step_us = (double)uapi_tcxo_get_us();
    for (int i = 1; i <= steps; i++) {
        if (g_abort_requested) {
            break;
        }

        next_step_us += step_time_us;
        uint64_t target_us = (uint64_t)(next_step_us + 0.5);
        if (!delay_until_us_interruptible(target_us)) {
            break;
        }

        uint64_t now_us = uapi_tcxo_get_us();
        if (now_us > target_us) {
            uint64_t late_us = now_us - target_us;
            record_late_sample(late_us);
            if (late_us >= SLE_JOB_MOTION_LATE_WARN_US) {
                unsigned long slipped = (unsigned long)((double)late_us / step_time_us);
                g_missed_sample_count += (slipped > 0UL) ? slipped : 1UL;
                next_step_us = (double)now_us;
            }
        }

        double fraction = (double)i / (double)steps;
        g_current_x = start_x + dx * fraction;
        g_current_y = start_y + dy * fraction;
        write_current_position(false);

        kick_watchdog_periodic(now_us, false);
        if ((i % 200) == 0) {
            update_activity();
        }
    }

    if (!g_abort_requested) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position(false);
    }

    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    g_abort_requested = false;
    g_motion_active = false;

    g_move_count++;
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
    uint32_t wall_ms = (uint32_t)uapi_systick_get_ms() - move_start_ms;
    if (wall_ms >= SLE_JOB_MOTION_MOVE_SLOW_MS) {
        uint32_t now_ms = (uint32_t)uapi_systick_get_ms();
        if (g_last_move_slow_log_ms != 0U &&
            (uint32_t)(now_ms - g_last_move_slow_log_ms) < SLE_JOB_MOTION_MOVE_SLOW_LOG_PERIOD_MS) {
            return;
        }
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
#endif
}

void sle_job_motion_executor_init(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_command_active = false;
    g_motion_active = false;
    g_abort_requested = false;
    g_last_activity_ms = 0;
    g_queue_head = 0;
    g_queue_tail = 0;
    g_worker_started = false;
    g_output_armed = false;
    g_enqueued_count = 0;
    g_executed_count = 0;
    g_late_sample_count = 0;
    g_missed_sample_count = 0;
    g_motion_segment_count = 0;
    g_short_segment_count = 0;
    g_max_sample_late_us = 0;
    g_arm_count = 0;
    g_move_count = 0;
    g_dac_write_count = 0;
    g_dac_skip_count = 0;
#if SLE_JOB_MOTION_MOVE_SLOW_LOG_ENABLE
    g_last_move_slow_log_ms = 0;
#endif
    g_last_dac_x = 0;
    g_last_dac_y = 0;
    g_last_dac_valid = false;
    g_last_wdt_kick_us = 0;
    memset(g_motion_queue, 0, sizeof(g_motion_queue));
    if (osal_mutex_init(&g_queue_mutex) == OSAL_SUCCESS &&
        osal_sem_init(&g_queue_sem, 0) == OSAL_SUCCESS) {
        g_queue_ready = true;
    }
    write_current_position(true);
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
    if (!g_queue_ready || !g_worker_started || cmd == NULL) {
        return false;
    }

    while (!g_abort_requested) {
        osal_mutex_lock(&g_queue_mutex);
        uint16_t next = (uint16_t)((g_queue_head + 1U) % SLE_JOB_MOTION_QUEUE_SIZE);
        if (next != g_queue_tail) {
            memcpy(&g_motion_queue[g_queue_head], cmd, sizeof(sle_job_motion_cmd_t));
            g_queue_head = next;
            g_enqueued_count++;
            osal_mutex_unlock(&g_queue_mutex);
            osal_sem_up(&g_queue_sem);
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
}

static int sle_job_motion_executor_task(void *arg)
{
    (void)arg;

    sle_job_motion_cmd_t cmd;
    while (1) {
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
    if (cmd == NULL) {
        return;
    }

    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    switch (cmd->cmd) {
        case SLE_JOB_CMD_G0_MOVE:
            arm_output_if_needed();
            laser_enable(false);
            perform_move(cmd->target_x, cmd->target_y, SLE_JOB_G0_FEED_RATE, false);
            break;
        case SLE_JOB_CMD_G1_MOVE: {
            double feed_rate = cmd->feed_rate;
            bool laser_on = cmd_uses_laser(cmd);
            arm_output_if_needed();
            if (laser_on && feed_rate > SLE_JOB_MARKING_FEED_RATE_MAX) {
                feed_rate = SLE_JOB_MARKING_FEED_RATE_MAX;
            }
            if (laser_on) {
                if (!laser_is_enabled() || laser_get_power() != cmd->laser_pwr) {
                    laser_set_power(cmd->laser_pwr);
                }
                if (!laser_is_enabled()) {
                    laser_enable(true);
                }
            } else {
                laser_enable(false);
            }
            perform_move(cmd->target_x, cmd->target_y, feed_rate, laser_on);
            if (!laser_on) {
                laser_enable(false);
            }
            break;
        }
        case SLE_JOB_CMD_LASER_ON:
            laser_set_power(cmd->laser_pwr);
            laser_enable(cmd->laser_pwr > 0);
            update_activity();
            break;
        case SLE_JOB_CMD_LASER_OFF:
            laser_enable(false);
            laser_set_power(0);
            update_activity();
            break;
        case SLE_JOB_CMD_SET_ORIGIN:
            laser_force_off();
            sle_job_motion_executor_set_origin();
            break;
        case SLE_JOB_CMD_EMERGENCY_STOP:
            sle_job_motion_executor_request_abort();
            sle_job_motion_executor_flush();
            laser_force_off();
            update_activity();
            break;
        default:
            break;
    }
    g_executed_count++;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void sle_job_motion_executor_set_origin(void)
{
    laser_force_off();
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_abort_requested = false;
    write_current_position(true);
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void sle_job_motion_executor_request_abort(void)
{
    g_abort_requested = true;
}

void sle_job_motion_executor_clear_abort(void)
{
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
    return g_abort_requested;
}

unsigned long sle_job_motion_executor_enqueued_count(void)
{
    return g_enqueued_count;
}

unsigned long sle_job_motion_executor_executed_count(void)
{
    return g_executed_count;
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
