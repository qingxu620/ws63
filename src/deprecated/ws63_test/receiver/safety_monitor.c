/**
 * @file safety_monitor.c
 * @brief 激光安全监控实现
 *        监控 SLE 连接状态，超时自动关闭激光
 */
#include "safety_monitor.h"
#include "config.h"
#include "laser_ctrl.h"
#include "cmd_queue.h"
#include "soc_osal.h"
#include "systick.h"

/*
 * 这些时间戳会在 SLE 回调线程、插补线程和安全线程之间共享。
 * WS63 ACore 为 32-bit，跨线程读写 uint64_t 存在撕裂风险，可能导致“仍在收心跳却误判超时”。
 * 这里统一改为 uint32_t 毫秒计数，原子读写足够覆盖当前毫秒级超时窗口。
 */
static volatile uint32_t g_last_sle_time = 0;
static volatile uint32_t g_last_cmd_time = 0;
static volatile uint32_t g_connect_time = 0;
static volatile bool g_sle_connected = false;
static volatile bool g_sle_timeout_handled = false;
static volatile bool g_sle_rx_after_connect = false;
static volatile uint8_t g_timeout_confirm_count = 0;

static inline uint32_t safety_now_ms(void)
{
    return (uint32_t)uapi_systick_get_ms();
}

static inline uint32_t safety_elapsed_ms(uint32_t now, uint32_t since)
{
    return (uint32_t)(now - since);
}

static bool safety_is_motion_active(uint32_t now)
{
    /* 任意有效 SLE 业务包(含心跳)最近到达，视为链路活跃：
     * 这样在“已连接但无运动”的空载阶段，不会退化到过短的 idle 超时阈值。 */
    if ((g_last_sle_time != 0U) && (safety_elapsed_ms(now, g_last_sle_time) < SAFETY_SLE_TIMEOUT_ACTIVE_MS)) {
        return true;
    }

    if (laser_is_enabled()) {
        return true;
    }

    if (cmd_queue_free_count() < (CMD_QUEUE_SIZE - 1)) {
        return true;
    }

    return (g_last_cmd_time != 0U) && (safety_elapsed_ms(now, g_last_cmd_time) < SAFETY_SLE_TIMEOUT_ACTIVE_MS);
}

void safety_update_last_sle_time(void)
{
    g_last_sle_time = safety_now_ms();
    g_sle_rx_after_connect = true;
    g_sle_timeout_handled = false;
    g_timeout_confirm_count = 0;
}

void safety_update_last_cmd_time(void)
{
    g_last_cmd_time = safety_now_ms();
}

void safety_set_sle_connected(bool connected)
{
    uint32_t now = safety_now_ms();
    g_sle_connected = connected;
    if (connected) {
        g_connect_time = now;
        g_last_sle_time = now;
        g_sle_rx_after_connect = false;
        g_sle_timeout_handled = false;
        g_timeout_confirm_count = 0;
        safety_update_last_cmd_time();
    } else {
        g_sle_rx_after_connect = false;
        g_sle_timeout_handled = false;
        g_timeout_confirm_count = 0;
    }
}

int task_safety_entry(void *arg)
{
    (void)arg;

    osal_printk("[safety] monitor task started\r\n");

    while (1) {
        uint32_t now = safety_now_ms();

        /* 检查 SLE 连接超时 */
        if (g_sle_connected && !g_sle_timeout_handled) {
            if (!g_sle_rx_after_connect && (safety_elapsed_ms(now, g_connect_time) <= SAFETY_SLE_CONNECT_GRACE_MS)) {
                g_timeout_confirm_count = 0;
                osal_msleep(SAFETY_CHECK_INTERVAL_MS);
                continue;
            }

            bool motion_active = safety_is_motion_active(now);
            uint32_t timeout_ms = motion_active ? SAFETY_SLE_TIMEOUT_ACTIVE_MS : SAFETY_SLE_TIMEOUT_MS;
            uint32_t elapsed_ms = safety_elapsed_ms(now, g_last_sle_time);
            if (elapsed_ms > timeout_ms) {
                if (g_timeout_confirm_count < 0xFFU) {
                    g_timeout_confirm_count++;
                }
                if (g_timeout_confirm_count >= SAFETY_TIMEOUT_CONFIRM_COUNT) {
                    osal_printk(
                        "[safety] SLE timeout confirmed! elapsed=%u threshold=%u confirm=%u motion=%u "
                        "rx_after_connect=%u Laser OFF\r\n",
                        elapsed_ms, timeout_ms, g_timeout_confirm_count, motion_active ? 1U : 0U,
                        g_sle_rx_after_connect ? 1U : 0U);
                    cmd_queue_flush();
                    laser_force_off();
                    g_sle_timeout_handled = true;
                }
            } else {
                g_timeout_confirm_count = 0;
            }
        } else {
            g_timeout_confirm_count = 0;
        }

        osal_msleep(SAFETY_CHECK_INTERVAL_MS);
    }

    return 0;
}

void safety_monitor_init(void)
{
    uint32_t now = safety_now_ms();

    /* 确保激光默认关闭 */
    laser_force_off();
    g_sle_connected = false;
    g_sle_timeout_handled = false;
    g_last_sle_time = now;
    g_last_cmd_time = now;
    g_connect_time = now;
    g_sle_rx_after_connect = false;
    g_timeout_confirm_count = 0;
}
