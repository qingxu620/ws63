/**
 * @file motion_executor.c
 * @brief Local synchronous motion executor.
 */
#include "motion_executor.h"
#include "config.h"
#include "dac8562.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "soc_osal.h"
#include "systick.h"
#include "tcxo.h"
#include "watchdog.h"
#include <math.h>
#include <string.h>

static double g_current_x = 0.0;
static double g_current_y = 0.0;
static volatile bool g_motion_active = false;
static volatile bool g_abort_requested = false;
static unsigned long g_last_activity_ms = 0;
static motion_cmd_t g_motion_queue[MOTION_QUEUE_SIZE];
static volatile uint16_t g_queue_head = 0;
static volatile uint16_t g_queue_tail = 0;
static osal_mutex g_queue_mutex;
static osal_semaphore g_queue_sem;
static bool g_queue_ready = false;
static uint64_t g_last_wdt_kick_us = 0;

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
    if (value > (double)DAC_MAX) {
        value = (double)DAC_MAX;
    }
    return (uint16_t)(value + 0.5);
}

static void write_current_position(void)
{
    dac8562_write_xy(mm_to_dac(g_current_x, BEILV_X), mm_to_dac(g_current_y, BEILV_Y));
}

static void update_activity(void)
{
    g_last_activity_ms = (unsigned long)uapi_systick_get_ms();
}

static void kick_watchdog_periodic(uint64_t now_us, bool force)
{
    if (force || g_last_wdt_kick_us == 0 ||
        (now_us - g_last_wdt_kick_us) >= MOTION_WDT_KICK_INTERVAL_US) {
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
        uint32_t chunk_us = (remain_us > 2000U) ? 2000U : (uint32_t)remain_us;
        if (g_abort_requested) {
            return false;
        }
        if (chunk_us > 0U) {
            uapi_tcxo_delay_us(chunk_us);
        }
    }

    return false;
}

static void perform_move(double target_x, double target_y, double feed_rate_mm_min)
{
    g_motion_active = true;
    g_abort_requested = false;
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);

    target_x = clamp_axis(target_x, GALVO_X_MIN_MM, GALVO_X_MAX_MM);
    target_y = clamp_axis(target_y, GALVO_Y_MIN_MM, GALVO_Y_MAX_MM);

    double start_x = g_current_x;
    double start_y = g_current_y;
    double dx = target_x - start_x;
    double dy = target_y - start_y;
    double distance = sqrt(dx * dx + dy * dy);

    if (distance < STEP_NUM) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position();
        gcode_processor_note_executed(g_current_x, g_current_y);
        update_activity();
        kick_watchdog_periodic(uapi_tcxo_get_us(), true);
        g_motion_active = false;
        return;
    }

    double feed_sec = feed_rate_mm_min / 60.0;
    if (feed_sec < 0.1) {
        feed_sec = 0.1;
    }

    int steps = (int)(distance / STEP_NUM);
    if (steps < 1) {
        steps = 1;
    }

    double step_dx = dx / steps;
    double step_dy = dy / steps;
    uint64_t step_time_us = (uint64_t)((((distance / feed_sec) * 1000000.0) / steps) + 0.5);
    if (step_time_us < 1U) {
        step_time_us = 1U;
    }

    uint64_t next_step_us = uapi_tcxo_get_us();
    for (int i = 1; i <= steps; i++) {
        if (g_abort_requested) {
            break;
        }

        g_current_x = start_x + step_dx * i;
        g_current_y = start_y + step_dy * i;
        write_current_position();
        kick_watchdog_periodic(uapi_tcxo_get_us(), false);

        next_step_us += step_time_us;
        if (!delay_until_us_interruptible(next_step_us)) {
            break;
        }
        if ((i % 200) == 0) {
            update_activity();
            osal_yield();
        }
    }

    if (!g_abort_requested) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position();
    }

    gcode_processor_note_executed(g_current_x, g_current_y);
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    g_abort_requested = false;
    g_motion_active = false;
}

void motion_executor_init(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_motion_active = false;
    g_abort_requested = false;
    g_last_activity_ms = 0;
    g_queue_head = 0;
    g_queue_tail = 0;
    g_last_wdt_kick_us = 0;
    memset(g_motion_queue, 0, sizeof(g_motion_queue));
    if (osal_mutex_init(&g_queue_mutex) == OSAL_SUCCESS &&
        osal_sem_init(&g_queue_sem, 0) == OSAL_SUCCESS) {
        g_queue_ready = true;
    }
    write_current_position();
}

static bool motion_queue_pop(motion_cmd_t *cmd)
{
    if (!g_queue_ready || cmd == NULL) {
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

    memcpy(cmd, &g_motion_queue[g_queue_tail], sizeof(motion_cmd_t));
    g_queue_tail = (uint16_t)((g_queue_tail + 1U) % MOTION_QUEUE_SIZE);
    osal_mutex_unlock(&g_queue_mutex);
    return true;
}

static uint16_t motion_queue_used_locked(void)
{
    if (g_queue_head >= g_queue_tail) {
        return (uint16_t)(g_queue_head - g_queue_tail);
    }
    return (uint16_t)(MOTION_QUEUE_SIZE - g_queue_tail + g_queue_head);
}

bool motion_executor_enqueue(const motion_cmd_t *cmd)
{
    if (!g_queue_ready || cmd == NULL) {
        return false;
    }

    while (!g_abort_requested) {
        osal_mutex_lock(&g_queue_mutex);
        uint16_t next = (uint16_t)((g_queue_head + 1U) % MOTION_QUEUE_SIZE);
        if (next != g_queue_tail) {
            memcpy(&g_motion_queue[g_queue_head], cmd, sizeof(motion_cmd_t));
            g_queue_head = next;
            osal_mutex_unlock(&g_queue_mutex);
            osal_sem_up(&g_queue_sem);
            return true;
        }
        osal_mutex_unlock(&g_queue_mutex);
        osal_msleep(1);
    }

    return false;
}

void motion_executor_flush(void)
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
}

static int motion_executor_task(void *arg)
{
    (void)arg;

    motion_cmd_t cmd;
    while (1) {
        if (motion_queue_pop(&cmd)) {
            motion_executor_execute(&cmd);
        }
    }

    return 0;
}

errcode_t motion_executor_start_task(void)
{
    osal_task *task = osal_kthread_create(motion_executor_task, NULL, "laser_motion", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_MOTION) != OSAL_SUCCESS) {
        osal_printk("[laser single] set motion priority failed\r\n");
    }
    osal_kfree(task);
    return ERRCODE_SUCC;
}

void motion_executor_execute(const motion_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    switch (cmd->cmd) {
        case CMD_G0_MOVE:
            perform_move(cmd->target_x, cmd->target_y, G0_FEED_RATE);
            break;
        case CMD_G1_MOVE: {
            double feed_rate = cmd->feed_rate;
            if ((cmd->flags & FLAG_LASER_ON) && feed_rate > MARKING_FEED_RATE_MAX) {
                feed_rate = MARKING_FEED_RATE_MAX;
            }
            perform_move(cmd->target_x, cmd->target_y, feed_rate);
            break;
        }
        case CMD_LASER_ON:
            laser_set_power(cmd->laser_pwr);
            laser_enable(true);
            update_activity();
            break;
        case CMD_LASER_OFF:
            laser_enable(false);
            update_activity();
            break;
        case CMD_SET_ORIGIN:
            motion_executor_set_origin();
            break;
        case CMD_EMERGENCY_STOP:
            motion_executor_request_abort();
            laser_enable(false);
            laser_set_power(0);
            update_activity();
            break;
        default:
            break;
    }
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void motion_executor_set_origin(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_abort_requested = false;
    write_current_position();
    gcode_processor_note_executed(g_current_x, g_current_y);
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void motion_executor_request_abort(void)
{
    g_abort_requested = true;
}

double motion_executor_get_x(void)
{
    return g_current_x;
}

double motion_executor_get_y(void)
{
    return g_current_y;
}

bool motion_executor_is_busy(void)
{
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

unsigned long motion_executor_last_activity_ms(void)
{
    return g_last_activity_ms;
}
