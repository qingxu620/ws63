/**
 * @file main.c
 * @brief Integrated RX route-based R5D coexist experiment entry.
 */
#include "app_init.h"
#include "boot_policy.h"
#include "dac8562.h"
#include "laser_ctrl.h"
#include "route_manager.h"
#include "soc_osal.h"

static void laser_rx_unified_entry(void)
{
    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_INTEGRATED] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_INTEGRATED] laser init failed: 0x%x\r\n", ret);
        return;
    }

    laser_force_off();
    route_manager_init();

#if defined(CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
    if (!route_manager_set_active(RX_ROUTE_SLE_JOB)) {
        osal_printk("[RX_INTEGRATED] start sle_job failed\r\n");
        laser_force_off();
        return;
    }
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
    if (rx_boot_policy_start() != ERRCODE_SUCC) {
        osal_printk("[RX_INTEGRATED] persistent advertising monitor failed, SLE_JOB remains active\r\n");
    }
#endif
#endif
}

app_run(laser_rx_unified_entry);
