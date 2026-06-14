/**
 * @file main.c
 * @brief RX board: SLE job receiver, local G-code executor, laser controller.
 */
#include "app_init.h"
#include "common_def.h"
#include "dac8562.h"
#include "gcode_processor.h"
#include "job_manager.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "sle_job_server.h"
#include "soc_osal.h"

static void laser_sle_job_rx_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser SLE Job RX\r\n");
    osal_printk("  mode: receive full job, then local execute\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] dac init failed: 0x%x\r\n", ret);
        return;
    }
    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] laser init failed: 0x%x\r\n", ret);
        return;
    }

    gcode_processor_init();
    motion_executor_init();
    job_manager_init();
    sle_job_server_set_packet_cb(job_manager_on_packet);
    sle_job_server_set_disconnect_cb(job_manager_on_disconnect);

    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] motion task failed: 0x%x\r\n", ret);
        return;
    }
    ret = sle_job_server_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] SLE server init failed: 0x%x\r\n", ret);
        job_manager_safe_stop("sle-init-fail");
        return;
    }

    osal_printk("[JOB_RX] ready, cache awaits JOB_BEGIN\r\n");
}

app_run(laser_sle_job_rx_entry);
