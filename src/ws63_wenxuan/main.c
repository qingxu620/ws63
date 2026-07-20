/**
 * @file main.c
 * @brief Wenxuan phone-enabled unified RX entry.
 */
#include "app_init.h"
#include "boot_policy.h"
#include "dac8563.h"
#include "laser_ctrl.h"
#include "route_manager.h"
#include "soc_osal.h"

#define RX_WENXUAN_FIRMWARE_PACKAGE "ws63-liteos-app_wenxuan_all.fwpkg"
#define RX_PHONE_INTEGRATION_VERSION "phone-rx-v7-20260719"

#if defined(CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE)
#define RX_PHONE_ADMISSION "1"
#else
#define RX_PHONE_ADMISSION "0"
#endif

static void laser_rx_wenxuan_entry(void)
{
    osal_printk("[FW_ID] board=RX firmware=%s app=ws63_wenxuan role=wenxuan-rx phase=R5D "
                "routes=sle_job,wifi,uart phone_integration=%s phone_admission=%s "
                "ssap_write_rsp=conditional notify_desc=client_config cccd_write=1 "
                "cccd_route=filtered phone_max=1 phone_data_len=512 phone_phy_mcs_tune=0 "
                "fixed_link_tune=1 "
                "phone_adv_restart=0\r\n",
                RX_WENXUAN_FIRMWARE_PACKAGE,
                RX_PHONE_INTEGRATION_VERSION,
                RX_PHONE_ADMISSION);

    errcode_t ret = dac8563_init();
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

app_run(laser_rx_wenxuan_entry);
