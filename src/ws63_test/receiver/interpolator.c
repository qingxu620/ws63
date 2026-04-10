/**
 * @file interpolator.c
 * @brief 插补引擎实现 — 从 Arduino performMove() 移植
 *        使用 osal_udelay() + osal_msleep() 组合实现平滑延时
 */
#include "interpolator.h"
#include "config.h"
#include "protocol.h"
#include "dac8562.h"
#include "cmd_queue.h"
#include "laser_ctrl.h"
#include "safety_monitor.h"
#include "soc_osal.h"
#include <math.h>

/* ================= 全局状态 ================= */
static double g_current_x = 0.0;
static double g_current_y = 0.0;
static volatile bool g_motion_active = false;

/* ================= 坐标转换 ================= */
static inline uint16_t mm_to_dac(double mm, double scale)
{
    double val = mm * scale;
    if (val < 0.0)
        val = 0.0;
    if (val > (double)DAC_MAX)
        val = (double)DAC_MAX;
    return (uint16_t)(val + 0.5);
}

static inline uint16_t x_mm_to_dac(double mm)
{
    return mm_to_dac(mm, BEILV_X);
}

static inline uint16_t y_mm_to_dac(double mm)
{
    return mm_to_dac(mm, BEILV_Y);
}

static inline double clamp_axis_mm(double mm, double min_mm, double max_mm)
{
    if (mm < min_mm) {
        return min_mm;
    }
    if (mm > max_mm) {
        return max_mm;
    }
    return mm;
}

static inline void write_current_position_to_dac(void)
{
    dac8562_write_xy(x_mm_to_dac(g_current_x), y_mm_to_dac(g_current_y));
}

static inline void interp_delay_us(unsigned int delay_us)
{
    if (delay_us >= 1000U) {
        osal_msleep(delay_us / 1000U);
        delay_us %= 1000U;
    }
    if (delay_us > 0U) {
        osal_udelay(delay_us);
    }
}

/* ================= 核心插补函数 ================= */
void perform_move(double target_x, double target_y, double feed_rate_mm_min)
{
    g_motion_active = true;

    double clamped_x = clamp_axis_mm(target_x, GALVO_X_MIN_MM, GALVO_X_MAX_MM);
    double clamped_y = clamp_axis_mm(target_y, GALVO_Y_MIN_MM, GALVO_Y_MAX_MM);
    if (clamped_x != target_x || clamped_y != target_y) {
        osal_printk("[interpolator] target clamped: (%.3f, %.3f)->(%.3f, %.3f)\r\n", target_x, target_y, clamped_x,
                    clamped_y);
    }
    target_x = clamped_x;
    target_y = clamped_y;

    double dx = target_x - g_current_x;
    double dy = target_y - g_current_y;
    double distance = sqrt(dx * dx + dy * dy);

    /* 距离太小，直接跳到目标点 */
    if (distance < STEP_NUM) {
        g_current_x = target_x;
        g_current_y = target_y;
        write_current_position_to_dac();
        g_motion_active = false;
        return;
    }

    /* 速度转换 mm/min → mm/sec */
    double feed_sec = feed_rate_mm_min / 60.0;
    if (feed_sec < 0.1)
        feed_sec = 0.1;

    /* 计算步数和每步增量 */
    int steps = (int)(distance / STEP_NUM);
    if (steps < 1)
        steps = 1;

    double step_dx = dx / steps;
    double step_dy = dy / steps;

    /* 计算每步延时 (微秒) */
    double total_time_sec = distance / feed_sec;
    double step_time_us = (total_time_sec * 1000000.0) / steps;
    if (step_time_us < 1)
        step_time_us = 1;

    unsigned int delay_us = (unsigned int)step_time_us;

    safety_update_last_cmd_time();

    for (int i = 1; i <= steps; i++) {
        g_current_x += step_dx;
        g_current_y += step_dy;

        /* 更新 DAC 输出 */
        write_current_position_to_dac();

        /* 延时拆分: 毫秒睡眠 + 微秒补偿 */
        interp_delay_us(delay_us);

        /*
         * 每 INTERP_UNLOCK_INTERVAL 步更新活动时间并让出一次 CPU
         * 保证 SLE/安全任务有调度机会，避免长轨迹误判超时
         */
        if (i % INTERP_UNLOCK_INTERVAL == 0) {
            safety_update_last_cmd_time();
            osal_yield();
        }
    }

    /* 修正终点坐标 (消除浮点累积误差) */
    g_current_x = target_x;
    g_current_y = target_y;
    write_current_position_to_dac();
    safety_update_last_cmd_time();
    g_motion_active = false;
}

/* ================= 状态接口 ================= */

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
    return g_motion_active;
}

void interpolator_set_origin(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    write_current_position_to_dac();
    g_motion_active = false;
}

void interpolator_init(void)
{
    g_current_x = 0.0;
    g_current_y = 0.0;
    g_motion_active = false;
    write_current_position_to_dac();
}

/* ================= 插补任务入口 ================= */
int task_interpolator_entry(void *arg)
{
    (void)arg;
    motion_cmd_t cmd;

    osal_printk("[interpolator] task started\r\n");

    while (1) {
        /* 阻塞等待命令 */
        if (!cmd_queue_pop(&cmd)) {
            continue;
        }

        /* 处理命令 */
        switch (cmd.cmd) {
            case CMD_G0_MOVE:
                /* G0 快速移动 — 使用最高速度 */
                perform_move(cmd.target_x, cmd.target_y, G0_FEED_RATE);
                break;

            case CMD_G1_MOVE:
                /* G1 直线插补 — 使用指定进给速度 */
                perform_move(cmd.target_x, cmd.target_y, cmd.feed_rate);
                break;

            case CMD_LASER_ON:
                laser_set_power(cmd.laser_pwr);
                laser_enable(true);
                break;

            case CMD_LASER_OFF:
                laser_enable(false);
                break;

            case CMD_SET_ORIGIN:
                interpolator_set_origin();
                break;

            default:
                break;
        }

        /* 更新安全看门狗 */
        safety_update_last_cmd_time();
    }

    return 0;
}
