/**
 * @file boot_policy.c
 * @brief R5B persistent SLE advertising boot policy.
 */
#include "boot_policy.h"
#include "common_def.h"
#include "laser_ctrl.h"
#include "route_manager.h"
#include "sle_job_route.h"
#include "soc_osal.h"

#define RX_BOOT_POLICY_POLL_MS 250U
#define RX_BOOT_POLICY_STACK_SIZE 0x1000U
#define RX_BOOT_POLICY_TASK_PRIORITY OSAL_TASK_PRIORITY_BELOW_MIDDLE

static volatile rx_boot_state_t g_boot_state = RX_BOOT_STATE_SLE_STARTING;

const char *rx_boot_policy_state_name(rx_boot_state_t state)
{
    switch (state) {
        case RX_BOOT_STATE_SLE_STARTING:
            return "SLE_STARTING";
        case RX_BOOT_STATE_SLE_ADVERTISING:
            return "SLE_ADVERTISING";
        case RX_BOOT_STATE_SLE_CONNECTED:
            return "SLE_CONNECTED";
        case RX_BOOT_STATE_SAFE:
            return "SAFE";
        default:
            return "UNKNOWN";
    }
}

rx_boot_state_t rx_boot_policy_get_state(void)
{
    return g_boot_state;
}

static int rx_boot_policy_task(void *arg)
{
    unused(arg);

    while (!sle_job_route_is_server_ready()) {
        if (sle_job_route_server_failed()) {
            laser_force_off();
            g_boot_state = RX_BOOT_STATE_SAFE;
            osal_printk("[RX_BOOT] sle_server_failed stay SAFE laser=OFF\r\n");
            return 0;
        }
        osal_msleep(RX_BOOT_POLICY_POLL_MS);
    }

    g_boot_state = RX_BOOT_STATE_SLE_ADVERTISING;
    osal_printk("[RX_BOOT] sle advertising persistent, waiting tx indefinitely\r\n");
    route_manager_dump_status();

    bool last_connected = false;
    while (1) {
        bool connected = sle_job_route_is_connected();
        if (connected != last_connected) {
            g_boot_state = connected ? RX_BOOT_STATE_SLE_CONNECTED :
                                       RX_BOOT_STATE_SLE_ADVERTISING;
            osal_printk("[RX_BOOT] tx_connected=%d stay SLE_JOB\r\n",
                        connected ? 1 : 0);
            route_manager_dump_status();
            last_connected = connected;
        }
        osal_msleep(RX_BOOT_POLICY_POLL_MS);
    }
}

errcode_t rx_boot_policy_start(void)
{
    g_boot_state = RX_BOOT_STATE_SLE_STARTING;
    osal_printk("[RX_BOOT] policy=SLE_PERSISTENT_ADVERTISING\r\n");

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(rx_boot_policy_task, NULL, "rx_boot_policy",
                                          RX_BOOT_POLICY_STACK_SIZE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[RX_BOOT] create policy task failed\r\n");
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, RX_BOOT_POLICY_TASK_PRIORITY) != OSAL_SUCCESS) {
        osal_printk("[RX_BOOT] set policy priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();
    return ERRCODE_SUCC;
}
