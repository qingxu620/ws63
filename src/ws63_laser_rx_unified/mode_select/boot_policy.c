/**
 * @file boot_policy.c
 * @brief R5D SLE + WiFi coexist demo boot policy.
 */
#include "boot_policy.h"
#include "common_def.h"
#include "laser_ctrl.h"
#include "legacy_wifi_route.h"
#include "route_manager.h"
#include "sle_job_route.h"
#include "soc_osal.h"

#define RX_BOOT_POLICY_POLL_MS 250U
#define RX_BOOT_POLICY_STACK_SIZE 0x1000U
#define RX_BOOT_POLICY_TASK_PRIORITY OSAL_TASK_PRIORITY_BELOW_MIDDLE

static volatile rx_boot_state_t g_boot_state = RX_BOOT_STATE_SLE_STARTING;
static volatile bool g_wifi_coexist_started = false;

const char *rx_boot_policy_state_name(rx_boot_state_t state)
{
    switch (state) {
        case RX_BOOT_STATE_SLE_STARTING:
            return "SLE_STARTING";
        case RX_BOOT_STATE_SLE_ADVERTISING:
            return "SLE_ADVERTISING";
        case RX_BOOT_STATE_SLE_CONNECTED:
            return "SLE_CONNECTED";
        case RX_BOOT_STATE_SLE_WIFI_COEXIST:
            return "SLE_WIFI_COEXIST";
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

static void rx_boot_start_wifi_coexist_once(void)
{
    if (g_wifi_coexist_started) {
        return;
    }

#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
    errcode_t ret = legacy_wifi_route_start();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_BOOT] WiFi coexist start failed: 0x%x\r\n", ret);
        laser_force_off();
        return;
    }
    g_wifi_coexist_started = true;
    g_boot_state = RX_BOOT_STATE_SLE_WIFI_COEXIST;
#else
    osal_printk("[RX_BOOT] WiFi coexist unavailable: transport disabled\r\n");
#endif
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
    rx_boot_start_wifi_coexist_once();

    bool last_connected = false;
    while (1) {
        bool connected = sle_job_route_is_connected();
        if (connected != last_connected) {
            g_boot_state = g_wifi_coexist_started ? RX_BOOT_STATE_SLE_WIFI_COEXIST :
                           (connected ? RX_BOOT_STATE_SLE_CONNECTED :
                                        RX_BOOT_STATE_SLE_ADVERTISING);
            last_connected = connected;
        }
        osal_msleep(RX_BOOT_POLICY_POLL_MS);
    }
}

errcode_t rx_boot_policy_start(void)
{
    g_boot_state = RX_BOOT_STATE_SLE_STARTING;
    g_wifi_coexist_started = false;

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
