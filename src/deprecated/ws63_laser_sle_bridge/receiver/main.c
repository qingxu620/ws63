/**
 * @file main.c
 * @brief Receiver for the SLE transparent serial bridge.
 *
 * The receiver is the only Grbl-compatible endpoint in this design.
 */
#include "app_init.h"
#include "bridge_rx_stats.h"
#include "common_def.h"
#include "config.h"
#include "dac8562.h"
#include "errcode.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "sle_receiver.h"
#include "soc_osal.h"
#include "stream_io.h"
#include "systick.h"

static volatile unsigned long g_rx_resp_generated = 0;
static volatile unsigned long g_rx_notify_retry = 0;
static volatile unsigned long g_rx_notify_fail = 0;
static volatile unsigned long g_rx_max_resp_delay_ms = 0;

static errcode_t bridge_stream_write(const void *data, uint16_t len)
{
    errcode_t ret = ERRCODE_FAIL;
    unsigned long start_ms = (unsigned long)uapi_systick_get_ms();

    g_rx_resp_generated++;

    for (uint8_t i = 0; i < SLE_BRIDGE_SEND_RETRY_MAX; i++) {
        ret = sle_receiver_send_response(data, len);
        if (ret == ERRCODE_SUCC) {
            unsigned long delay_ms = (unsigned long)uapi_systick_get_ms() - start_ms;
            if (delay_ms > g_rx_max_resp_delay_ms) {
                g_rx_max_resp_delay_ms = delay_ms;
            }
            return ret;
        }
        g_rx_notify_retry++;
        osal_msleep(SLE_BRIDGE_SEND_RETRY_DELAY_MS);
    }

    g_rx_notify_fail++;
    return ret;
}

void bridge_rx_stats_get(bridge_rx_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->resp_generated = g_rx_resp_generated;
    stats->notify_retry = g_rx_notify_retry;
    stats->notify_fail = g_rx_notify_fail;
    stats->max_resp_delay_ms = g_rx_max_resp_delay_ms;
}

static void create_task(const char *name, int (*entry)(void *), unsigned short prio)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(entry, NULL, name, TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[bridge rx] create %s failed\r\n", name);
        return;
    }
    if (osal_kthread_set_priority(task, prio) != OSAL_SUCCESS) {
        osal_printk("[bridge rx] set %s priority failed\r\n", name);
    }
    osal_kfree(task);
    osal_kthread_unlock();
}

static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    if (sle_receiver_init() != ERRCODE_SUCC) {
        osal_printk("[bridge rx] sle receiver init failed\r\n");
    }
    return 0;
}

static void laser_sle_bridge_rx_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser SLE Bridge RX\r\n");
    osal_printk("  mode: only Grbl endpoint\r\n");
    osal_printk("========================================\r\n");

    if (dac8562_init() != ERRCODE_SUCC) {
        osal_printk("[bridge rx] dac init failed\r\n");
        return;
    }
    if (laser_ctrl_init() != ERRCODE_SUCC) {
        osal_printk("[bridge rx] laser init failed\r\n");
        return;
    }

    gcode_processor_init();
    motion_executor_init();

    if (stream_io_init(bridge_stream_write) != ERRCODE_SUCC) {
        return;
    }

    create_task("bridge_stream", stream_io_task, TASK_PRIO_UART);

    if (motion_executor_start_task() != ERRCODE_SUCC) {
        osal_printk("[bridge rx] create motion task failed\r\n");
    }

    create_task("bridge_sle", sle_init_task, TASK_PRIO_UART + 1);

    osal_printk("[bridge rx] ready, waiting for SLE bridge\r\n");
}

app_run(laser_sle_bridge_rx_entry);
