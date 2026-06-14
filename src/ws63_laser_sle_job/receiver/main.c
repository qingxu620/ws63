/**
 * @file main.c
 * @brief RX board: SLE job receiver, local G-code executor, laser controller.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "dac8562.h"
#include "gcode_processor.h"
#include "job_manager.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "sle_job_server.h"
#include "soc_osal.h"

static int sle_init_task(void *arg)
{
    unused(arg);

    osal_printk("[JOB_RX_BOOT] sle init task start, delay for stack ready\r\n");
    osal_msleep(500);

    osal_printk("[JOB_RX_BOOT] sle server init begin\r\n");
    errcode_t ret = sle_job_server_init();
    osal_printk("[JOB_RX_BOOT] sle server init end ret=0x%x\r\n", ret);
    if (ret != ERRCODE_SUCC) {
        job_manager_safe_stop("sle-init-fail");
        return -1;
    }
    return 0;
}

static void laser_sle_job_rx_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser SLE Job RX\r\n");
    osal_printk("  mode: receive full job, then local execute\r\n");
    osal_printk("========================================\r\n");

    osal_printk("[JOB_RX_BOOT] dac init begin\r\n");
    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] dac init failed: 0x%x\r\n", ret);
        return;
    }
    osal_printk("[JOB_RX_BOOT] laser init begin\r\n");
    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] laser init failed: 0x%x\r\n", ret);
        return;
    }

    osal_printk("[JOB_RX_BOOT] gcode/motion/job init begin\r\n");
    gcode_processor_init();
    motion_executor_init();
    job_manager_init();
    osal_printk("[JOB_RX_BOOT] callbacks set begin\r\n");
    sle_job_server_set_packet_cb(job_manager_on_packet);
    sle_job_server_set_disconnect_cb(job_manager_on_disconnect);

    osal_printk("[JOB_RX_BOOT] motion task start begin\r\n");
    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_RX] motion task failed: 0x%x\r\n", ret);
        return;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(sle_init_task, NULL, "job_sle_init", TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[JOB_RX] create sle init task failed\r\n");
        job_manager_safe_stop("sle-task-fail");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_SLE) != OSAL_SUCCESS) {
        osal_printk("[JOB_RX] set sle priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[JOB_RX] boot tasks created, waiting for SLE ready\r\n");
}

app_run(laser_sle_job_rx_entry);
