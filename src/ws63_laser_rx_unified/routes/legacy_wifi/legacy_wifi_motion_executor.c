/**
 * @file motion_executor.c
 * @brief Local synchronous motion executor.
 */
#include "legacy_wifi_motion_executor.h"
#include "legacy_wifi_config.h"
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
static unsigned long g_last_activity_ms = 0;
static legacy_wifi_motion_cmd_t g_motion_queue[LEGACY_WIFI_MOTION_QUEUE_SIZE];
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
static volatile unsigned long g_dac_skip_count = 0;
static volatile unsigned long g_timer_wait_count = 0;
static volatile unsigned long g_timer_fail_count = 0;
static volatile unsigned long g_timer_wake_late_max_us = 0;
static timer_handle_t g_motion_timer = NULL;
static osal_semaphore g_motion_timer_sem;
static bool g_motion_timer_sem_ready = false;
static volatile bool g_motion_timer_waiting = false;
static bool g_motion_timer_ready = false;
static uint16_t g_last_dac_x = 0;
static uint16_t g_last_dac_y = 0;
static bool g_last_dac_valid = false;
static uint64_t g_g0_settle_until_us = 0;
static uint64_t g_last_wdt_kick_us = 0;

#define MOTION_LATE_WARN_US 100U

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
    if (value > (double)LEGACY_WIFI_DAC_MAX) {
        value = (double)LEGACY_WIFI_DAC_MAX;
    }
    return (uint16_t)(value + 0.5);
}

static bool write_current_position(bool force)
{
    uint16_t x = mm_to_dac(g_current_x, LEGACY_WIFI_BEILV_X);
    uint16_t y = mm_to_dac(g_current_y, LEGACY_WIFI_BEILV_Y);
    if (!force && g_last_dac_valid && x == g_last_dac_x && y == g_last_dac_y) {
        g_dac_skip_count++;
        return true;
    }

    errcode_t ret = dac8563_write_xy(x, y);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[WIFI_MOTION] DAC write failed ret=0x%x, abort motion\r\n", ret);
        laser_force_off();
        g_abort_requested = true;
        g_output_armed = false;
        g_last_dac_valid = false;
        return false;
    }
    g_last_dac_x = x;
    g_last_dac_y = y;
    g_last_dac_valid = true;
    return true;
}

static bool arm_output_if_needed(void)
{
    if (g_output_armed) {
        return true;
    }

    laser_force_off();
    errcode_t ret = dac8563_recover();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[WIFI_MOTION] DAC recover failed ret=0x%x\r\n", ret);
        laser_force_off();
        g_abort_requested = true;
        g_last_dac_valid = false;
        return false;
    }
    if (!write_current_position(true)) {
        return false;
    }
    g_output_armed = true;
    return true;
}

static void update_activity(void)
{
    g_last_activity_ms = (unsigned long)uapi_systick_get_ms();
}

static void kick_watchdog_periodic(uint64_t now_us, bool force)
{
    if (force || g_last_wdt_kick_us == 0 ||
        (now_us - g_last_wdt_kick_us) >= LEGACY_WIFI_MOTION_WDT_KICK_INTERVAL_US) {
        (void)uapi_watchdog_kick();
        g_last_wdt_kick_us = now_us;
    }
}

static void motion_timer_callback(uintptr_t data)
{
    (void)data;
    g_motion_timer_waiting = false;
    osal_sem_up(&g_motion_timer_sem);
}

static bool wait_with_motion_timer(uint64_t target_us, uint64_t now_us)
{
    uint64_t remain_us = target_us - now_us;
    if (!g_motion_timer_ready || remain_us <= LEGACY_WIFI_MOTION_TIMER_THRESHOLD_US) {
        return false;
    }

    uint32_t timer_us = (uint32_t)(remain_us - LEGACY_WIFI_MOTION_TIMER_TAIL_US);
    g_motion_timer_waiting = true;
    errcode_t ret = uapi_timer_start(g_motion_timer, timer_us, motion_timer_callback, 0);
    if (ret != ERRCODE_SUCC) {
        g_motion_timer_waiting = false;
        g_timer_fail_count++;
        osal_printk("[WIFI_MOTION_TIMER_FAIL] op=start ret=0x%x wait_us=%u\r\n",
                    (unsigned int)ret, (unsigned int)timer_us);
        laser_force_off();
        g_abort_requested = true;
        return true;
    }

    g_timer_wait_count++;
    uint32_t timeout_ms = (timer_us + 999U) / 1000U +
                          LEGACY_WIFI_MOTION_TIMER_TIMEOUT_MARGIN_MS;
    int sem_ret = osal_sem_down_timeout(&g_motion_timer_sem, timeout_ms);
    uint64_t wake_us = uapi_tcxo_get_us();
    if (sem_ret != OSAL_SUCCESS) {
        (void)uapi_timer_stop(g_motion_timer);
        g_motion_timer_waiting = false;
        g_timer_fail_count++;
        osal_printk("[WIFI_MOTION_TIMER_FAIL] op=wait wait_us=%u timeout_ms=%u\r\n",
                    (unsigned int)timer_us, (unsigned int)timeout_ms);
        laser_force_off();
        g_abort_requested = true;
        return true;
    }

    uint64_t timer_target_us = target_us - LEGACY_WIFI_MOTION_TIMER_TAIL_US;
    if (wake_us > timer_target_us) {
        uint64_t late_us = wake_us - timer_target_us;
        if (late_us > g_timer_wake_late_max_us) {
            g_timer_wake_late_max_us = (late_us > UINT32_MAX) ? UINT32_MAX :
                                       (unsigned long)late_us;
        }
    }
    return true;
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
        if (wait_with_motion_timer(target_us, now_us)) {
            continue;
        }
        uint32_t chunk_us = (remain_us > LEGACY_WIFI_MOTION_TIMER_TAIL_US) ?
                            LEGACY_WIFI_MOTION_TIMER_TAIL_US : (uint32_t)remain_us;
        if (g_abort_requested) {
            return false;
        }
        if (chunk_us > 0U) {
            uapi_tcxo_delay_us(chunk_us);
        }
    }

    return false;
}

static bool wait_for_g0_settle_if_needed(void)
{
    uint64_t settle_until_us = g_g0_settle_until_us;
    if (settle_until_us == 0U) {
        return true;
    }
    if (!delay_until_us_interruptible(settle_until_us)) {
        laser_force_off();
        return false;
    }
    g_g0_settle_until_us = 0;
    return true;
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
    if (late_us >= MOTION_LATE_WARN_US) {
        g_late_sample_count++;
    }
}

static void perform_move(double target_x, double target_y, double feed_rate_mm_min,
                         double spatial_step_mm, bool laser_marking)
{
    g_motion_active = true;
    g_abort_requested = false;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);

    target_x = clamp_axis(target_x, LEGACY_WIFI_GALVO_X_MIN_MM, LEGACY_WIFI_GALVO_X_MAX_MM);
    target_y = clamp_axis(target_y, LEGACY_WIFI_GALVO_Y_MIN_MM, LEGACY_WIFI_GALVO_Y_MAX_MM);

    double start_x = g_current_x;
    double start_y = g_current_y;
    double dx = target_x - start_x;
    double dy = target_y - start_y;
    double distance = sqrt(dx * dx + dy * dy);

    if (distance <= 0.000001) {
        g_current_x = target_x;
        g_current_y = target_y;
        (void)write_current_position(false);
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
    bool short_segment = (distance <= spatial_step_mm ||
                          duration_us <= (double)LEGACY_WIFI_MOTION_SAMPLE_PERIOD_US);
    if (short_segment) {
        g_short_segment_count++;
    }
    if (laser_marking && short_segment && duration_us < (double)LEGACY_WIFI_MOTION_MIN_MARK_SEGMENT_US) {
        duration_us = (double)LEGACY_WIFI_MOTION_MIN_MARK_SEGMENT_US;
    }

    int time_steps = ceil_step_count(duration_us / (double)LEGACY_WIFI_MOTION_SAMPLE_PERIOD_US);
    int space_steps = ceil_step_count(distance / spatial_step_mm);
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
            if (late_us >= MOTION_LATE_WARN_US) {
                unsigned long slipped = (unsigned long)((double)late_us / step_time_us);
                g_missed_sample_count += (slipped > 0UL) ? slipped : 1UL;
                next_step_us = (double)now_us;
            }
        }

        double fraction = (double)i / (double)steps;
        g_current_x = start_x + dx * fraction;
        g_current_y = start_y + dy * fraction;
        if (!write_current_position(false)) {
            break;
        }

        kick_watchdog_periodic(now_us, false);
        if ((i % 200) == 0) {
            update_activity();
        }
    }

    if (!g_abort_requested) {
        g_current_x = target_x;
        g_current_y = target_y;
        (void)write_current_position(false);
    }

    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    g_abort_requested = false;
    g_motion_active = false;
}

void legacy_wifi_motion_executor_init(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_command_active = false;
    g_motion_active = false;
    g_abort_requested = false;
    g_last_activity_ms = 0;
    g_queue_head = 0;
    g_queue_tail = 0;
    g_output_armed = false;
    g_enqueued_count = 0;
    g_executed_count = 0;
    g_late_sample_count = 0;
    g_missed_sample_count = 0;
    g_motion_segment_count = 0;
    g_short_segment_count = 0;
    g_max_sample_late_us = 0;
    g_dac_skip_count = 0;
    g_timer_wait_count = 0;
    g_timer_fail_count = 0;
    g_timer_wake_late_max_us = 0;
    g_motion_timer_waiting = false;
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
    while (g_queue_ready &&
           osal_sem_down_timeout(&g_queue_sem, 0) == OSAL_SUCCESS) {
    }
    if (!g_motion_timer_sem_ready &&
        osal_sem_init(&g_motion_timer_sem, 0) == OSAL_SUCCESS) {
        g_motion_timer_sem_ready = true;
    }
    if (g_motion_timer_sem_ready && !g_motion_timer_ready &&
        uapi_timer_adapter(LEGACY_WIFI_MOTION_TIMER_INDEX, TIMER_2_IRQN,
                           LEGACY_WIFI_MOTION_TIMER_IRQ_PRIORITY) == ERRCODE_SUCC &&
        uapi_timer_create(LEGACY_WIFI_MOTION_TIMER_INDEX, &g_motion_timer) == ERRCODE_SUCC) {
        g_motion_timer_ready = true;
        osal_printk("[WIFI_MOTION_TIMER] ready index=%u threshold_us=%u tail_us=%u\r\n",
                    (unsigned int)LEGACY_WIFI_MOTION_TIMER_INDEX,
                    (unsigned int)LEGACY_WIFI_MOTION_TIMER_THRESHOLD_US,
                    (unsigned int)LEGACY_WIFI_MOTION_TIMER_TAIL_US);
    }
    if (!g_motion_timer_ready) {
        osal_printk("[WIFI_MOTION_TIMER] init failed\r\n");
    }
    while (g_motion_timer_sem_ready &&
           osal_sem_down_timeout(&g_motion_timer_sem, 0) == OSAL_SUCCESS) {
    }
    (void)write_current_position(true);
}

static bool motion_queue_pop(legacy_wifi_motion_cmd_t *cmd)
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

    memcpy(cmd, &g_motion_queue[g_queue_tail], sizeof(legacy_wifi_motion_cmd_t));
    g_queue_tail = (uint16_t)((g_queue_tail + 1U) % LEGACY_WIFI_MOTION_QUEUE_SIZE);
    g_command_active = true;
    osal_mutex_unlock(&g_queue_mutex);
    return true;
}

static uint16_t motion_queue_used_locked(void)
{
    if (g_queue_head >= g_queue_tail) {
        return (uint16_t)(g_queue_head - g_queue_tail);
    }
    return (uint16_t)(LEGACY_WIFI_MOTION_QUEUE_SIZE - g_queue_tail + g_queue_head);
}

static bool cmd_uses_laser(const legacy_wifi_motion_cmd_t *cmd)
{
    return cmd != NULL && cmd->cmd == LEGACY_WIFI_CMD_G1_MOVE && ((cmd->flags & LEGACY_WIFI_FLAG_LASER_ON) != 0) && cmd->laser_pwr > 0;
}

bool legacy_wifi_motion_executor_enqueue(const legacy_wifi_motion_cmd_t *cmd)
{
    if (!g_queue_ready || !g_worker_started || cmd == NULL) {
        return false;
    }

    while (!g_abort_requested) {
        osal_mutex_lock(&g_queue_mutex);
        uint16_t next = (uint16_t)((g_queue_head + 1U) % LEGACY_WIFI_MOTION_QUEUE_SIZE);
        if (next != g_queue_tail) {
            memcpy(&g_motion_queue[g_queue_head], cmd, sizeof(legacy_wifi_motion_cmd_t));
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

void legacy_wifi_motion_executor_flush(void)
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

static int legacy_wifi_motion_executor_task(void *arg)
{
    (void)arg;

    legacy_wifi_motion_cmd_t cmd;
    while (1) {
        if (motion_queue_pop(&cmd)) {
            legacy_wifi_motion_executor_execute(&cmd);
            g_command_active = false;
        } else {
            osal_msleep(1);
        }
    }

    return 0;
}

errcode_t legacy_wifi_motion_executor_start_task(void)
{
    if (!g_queue_ready) {
        return ERRCODE_FAIL;
    }
    if (g_worker_started) {
        return ERRCODE_SUCC;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(legacy_wifi_motion_executor_task, NULL, "laser_motion", LEGACY_WIFI_TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, LEGACY_WIFI_TASK_PRIO_MOTION) != OSAL_SUCCESS) {
        osal_printk("[laser single] set motion priority failed\r\n");
    }
    osal_kfree(task);
    g_worker_started = true;
    osal_kthread_unlock();
    return ERRCODE_SUCC;
}

void legacy_wifi_motion_executor_execute(const legacy_wifi_motion_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    switch (cmd->cmd) {
        case LEGACY_WIFI_CMD_G0_MOVE:
            if (!arm_output_if_needed()) {
                break;
            }
            laser_enable(false);
            perform_move(cmd->target_x, cmd->target_y, LEGACY_WIFI_G0_FEED_RATE,
                         LEGACY_WIFI_G0_STEP_NUM, false);
            if (!g_abort_requested) {
                g_g0_settle_until_us = uapi_tcxo_get_us() + LEGACY_WIFI_G0_SETTLE_US;
            }
            break;
        case LEGACY_WIFI_CMD_G1_MOVE: {
            double feed_rate = cmd->feed_rate;
            bool laser_on = cmd_uses_laser(cmd);
            if (!arm_output_if_needed()) {
                break;
            }
            if (laser_on && feed_rate > LEGACY_WIFI_MARKING_FEED_RATE_MAX) {
                feed_rate = LEGACY_WIFI_MARKING_FEED_RATE_MAX;
            }
            if (laser_on) {
                if (!wait_for_g0_settle_if_needed()) {
                    break;
                }
                if (!laser_is_enabled() || laser_get_power() != cmd->laser_pwr) {
                    laser_set_power(cmd->laser_pwr);
                }
                if (!laser_is_enabled()) {
                    laser_enable(true);
                }
            } else {
                laser_enable(false);
            }
            perform_move(cmd->target_x, cmd->target_y, feed_rate,
                         LEGACY_WIFI_STEP_NUM, laser_on);
            if (!laser_on) {
                laser_enable(false);
            }
            break;
        }
        case LEGACY_WIFI_CMD_LASER_ON:
            if (!wait_for_g0_settle_if_needed()) {
                break;
            }
            laser_set_power(cmd->laser_pwr);
            laser_enable(cmd->laser_pwr > 0);
            update_activity();
            break;
        case LEGACY_WIFI_CMD_LASER_OFF:
            /* M5 must close PWM and drive the laser pin physically low. */
            laser_force_off();
            update_activity();
            break;
        case LEGACY_WIFI_CMD_SET_ORIGIN:
            laser_force_off();
            legacy_wifi_motion_executor_set_origin();
            break;
        case LEGACY_WIFI_CMD_EMERGENCY_STOP:
            legacy_wifi_motion_executor_request_abort();
            legacy_wifi_motion_executor_flush();
            laser_force_off();
            update_activity();
            break;
        default:
            break;
    }
    g_executed_count++;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void legacy_wifi_motion_executor_set_origin(void)
{
    laser_force_off();
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_abort_requested = false;
    (void)write_current_position(true);
    g_g0_settle_until_us = g_abort_requested ? 0U :
                           uapi_tcxo_get_us() + LEGACY_WIFI_G0_SETTLE_US;
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void legacy_wifi_motion_executor_request_abort(void)
{
    g_abort_requested = true;
    g_g0_settle_until_us = 0;
    if (g_motion_timer_ready && g_motion_timer_waiting) {
        (void)uapi_timer_stop(g_motion_timer);
        g_motion_timer_waiting = false;
        osal_sem_up(&g_motion_timer_sem);
    }
}

double legacy_wifi_motion_executor_get_x(void)
{
    return g_current_x;
}

double legacy_wifi_motion_executor_get_y(void)
{
    return g_current_y;
}

bool legacy_wifi_motion_executor_is_busy(void)
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

uint16_t legacy_wifi_motion_executor_queue_depth(void)
{
    if (!g_queue_ready) {
        return 0;
    }

    osal_mutex_lock(&g_queue_mutex);
    uint16_t used = motion_queue_used_locked();
    osal_mutex_unlock(&g_queue_mutex);
    return used;
}

bool legacy_wifi_motion_executor_worker_started(void)
{
    return g_worker_started;
}

bool legacy_wifi_motion_executor_abort_requested(void)
{
    return g_abort_requested;
}

unsigned long legacy_wifi_motion_executor_enqueued_count(void)
{
    return g_enqueued_count;
}

unsigned long legacy_wifi_motion_executor_executed_count(void)
{
    return g_executed_count;
}

unsigned long legacy_wifi_motion_executor_last_activity_ms(void)
{
    return g_last_activity_ms;
}

unsigned long legacy_wifi_motion_executor_late_sample_count(void)
{
    return g_late_sample_count;
}

unsigned long legacy_wifi_motion_executor_missed_sample_count(void)
{
    return g_missed_sample_count;
}

unsigned long legacy_wifi_motion_executor_motion_segment_count(void)
{
    return g_motion_segment_count;
}

unsigned long legacy_wifi_motion_executor_short_segment_count(void)
{
    return g_short_segment_count;
}

unsigned long legacy_wifi_motion_executor_max_sample_late_us(void)
{
    return g_max_sample_late_us;
}

unsigned long legacy_wifi_motion_executor_dac_skip_count(void)
{
    return g_dac_skip_count;
}

unsigned long legacy_wifi_motion_executor_timer_wait_count(void)
{
    return g_timer_wait_count;
}

unsigned long legacy_wifi_motion_executor_timer_fail_count(void)
{
    return g_timer_fail_count;
}

unsigned long legacy_wifi_motion_executor_timer_wake_late_max_us(void)
{
    return g_timer_wake_late_max_us;
}
