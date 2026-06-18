/**
 * @file interpolator.c
 * @brief Receiver motion executor.
 *
 * The SLE protocol and cmd_queue remain unchanged, but the execution timing is
 * aligned with the proven single-board motion_executor flow.
 */
#include "interpolator.h"
#include "cmd_queue.h"
#include "config.h"
#include "dac8562.h"
#include "laser_ctrl.h"
#include "protocol.h"
#include "safety_monitor.h"
#include "soc_osal.h"
#include "tcxo.h"
#include "watchdog.h"
#include <math.h>

static double g_current_x = 0.0;
static double g_current_y = 0.0;
static volatile bool g_command_active = false;
static volatile bool g_motion_active = false;
static volatile bool g_abort_requested = false;
static bool g_output_armed = false;
static volatile unsigned long g_late_sample_count = 0;
static volatile unsigned long g_missed_sample_count = 0;
static volatile unsigned long g_motion_segment_count = 0;
static volatile unsigned long g_short_segment_count = 0;
static volatile unsigned long g_max_sample_late_us = 0;
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

static void arm_output_if_needed(void)
{
    if (g_output_armed) {
        return;
    }

    laser_force_off();
    dac8562_recover();
    write_current_position();
    g_output_armed = true;
}

static void update_activity(void)
{
    safety_update_last_cmd_time();
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
        if (chunk_us > 0U) {
            uapi_tcxo_delay_us(chunk_us);
            if (chunk_us >= 2000U) {
                osal_yield();
            }
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
    if (late_us >= MOTION_LATE_WARN_US) {
        g_late_sample_count++;
    }
}

static bool cmd_uses_laser(const motion_cmd_t *cmd)
{
    return cmd != NULL && cmd->cmd == CMD_G1_MOVE && ((cmd->flags & FLAG_LASER_ON) != 0) && cmd->laser_pwr > 0;
}

static void perform_move_internal(double target_x, double target_y, double feed_rate_mm_min, bool laser_marking)
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

    if (distance <= 0.000001) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position();
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
    bool short_segment = (distance <= STEP_NUM || duration_us <= (double)MOTION_SAMPLE_PERIOD_US);
    if (short_segment) {
        g_short_segment_count++;
    }
    if (laser_marking && short_segment && duration_us < (double)MOTION_MIN_MARK_SEGMENT_US) {
        duration_us = (double)MOTION_MIN_MARK_SEGMENT_US;
    }

    int time_steps = ceil_step_count(duration_us / (double)MOTION_SAMPLE_PERIOD_US);
    int space_steps = ceil_step_count(distance / STEP_NUM);
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
        write_current_position();

        kick_watchdog_periodic(now_us, false);
        if ((i % INTERP_UNLOCK_INTERVAL) == 0) {
            update_activity();
        }
    }

    if (!g_abort_requested) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position();
    }

    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    g_abort_requested = false;
    g_motion_active = false;
}

void perform_move(double target_x, double target_y, double feed_rate_mm_min)
{
    perform_move_internal(target_x, target_y, feed_rate_mm_min, false);
}

static void execute_cmd(const motion_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
    switch (cmd->cmd) {
        case CMD_G0_MOVE:
            arm_output_if_needed();
            laser_enable(false);
            perform_move_internal(cmd->target_x, cmd->target_y, G0_FEED_RATE, false);
            break;

        case CMD_G1_MOVE: {
            double feed_rate = cmd->feed_rate;
            bool laser_on = cmd_uses_laser(cmd);
            arm_output_if_needed();
            if (laser_on && feed_rate > MARKING_FEED_RATE_MAX) {
                feed_rate = MARKING_FEED_RATE_MAX;
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
            perform_move_internal(cmd->target_x, cmd->target_y, feed_rate, laser_on);
            if (!laser_on) {
                laser_enable(false);
            }
            break;
        }

        case CMD_LASER_ON:
            laser_set_power(cmd->laser_pwr);
            laser_enable(cmd->laser_pwr > 0);
            update_activity();
            break;

        case CMD_LASER_OFF:
            laser_enable(false);
            laser_set_power(0);
            update_activity();
            break;

        case CMD_SET_ORIGIN:
            laser_force_off();
            interpolator_set_origin();
            break;

        case CMD_EMERGENCY_STOP:
            interpolator_request_abort();
            cmd_queue_flush();
            laser_force_off();
            update_activity();
            break;

        default:
            break;
    }
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

double interpolator_get_x(void)
{
    return g_current_x;
}

double interpolator_get_y(void)
{
    return g_current_y;
}

bool interpolator_is_busy(void)
{
    if (g_command_active || g_motion_active) {
        return true;
    }
    return cmd_queue_free_count() < (CMD_QUEUE_SIZE - 1);
}

void interpolator_set_origin(void)
{
    laser_force_off();
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_abort_requested = false;
    write_current_position();
    update_activity();
    kick_watchdog_periodic(uapi_tcxo_get_us(), true);
}

void interpolator_request_abort(void)
{
    g_abort_requested = true;
}

void interpolator_init(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_command_active = false;
    g_motion_active = false;
    g_abort_requested = false;
    g_output_armed = false;
    g_late_sample_count = 0;
    g_missed_sample_count = 0;
    g_motion_segment_count = 0;
    g_short_segment_count = 0;
    g_max_sample_late_us = 0;
    g_last_wdt_kick_us = 0;
    write_current_position();
}

int task_interpolator_entry(void *arg)
{
    (void)arg;
    motion_cmd_t cmd;

    osal_printk("[interpolator] motion executor task started\r\n");

    while (1) {
        if (!cmd_queue_pop(&cmd)) {
            continue;
        }

        g_command_active = true;
        execute_cmd(&cmd);
        g_command_active = false;
        update_activity();
    }

    return 0;
}
