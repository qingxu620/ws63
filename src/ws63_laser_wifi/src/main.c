/**
 * @file main.c
 * @brief WiFi WS63 laser marker entry.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "dac8562.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "soc_osal.h"
#include "wifi_grbl_server.h"

static void laser_wifi_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 WiFi Laser Marker\r\n");
    osal_printk("  mode: SoftAP TCP Grbl endpoint\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wifi] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wifi] laser init failed: 0x%x\r\n", ret);
        return;
    }

    gcode_processor_init();
    motion_executor_init();

    ret = wifi_grbl_server_init();
    if (ret != ERRCODE_SUCC) {
        return;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(task_wifi_grbl_entry, NULL, "laser_wifi", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[laser wifi] create wifi task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_WIFI) != OSAL_SUCCESS) {
        osal_printk("[laser wifi] set wifi priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wifi] create motion task failed: 0x%x\r\n", ret);
    }

    osal_printk("[laser wifi] ready\r\n");
}

app_run(laser_wifi_entry);
