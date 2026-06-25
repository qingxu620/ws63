/**
 * @file main.c
 * @brief Single-board WS63 laser marker entry.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "dac8562.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "soc_osal.h"
#include "uart_handler.h"

static void laser_single_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Single Board Laser Marker\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser single] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser single] laser init failed: 0x%x\r\n", ret);
        return;
    }

    gcode_processor_init();
    motion_executor_init();

    ret = uart_handler_init();
    if (ret != ERRCODE_SUCC) {
        return;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(task_uart_rx_entry, NULL, "laser_uart", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[laser single] create uart task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_UART) != OSAL_SUCCESS) {
        osal_printk("[laser single] set uart priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser single] create motion task failed: 0x%x\r\n", ret);
    }

    osal_printk("[laser single] ready\r\n");
}

app_run(laser_single_entry);
