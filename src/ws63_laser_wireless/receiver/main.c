/**
 * @file main.c
 * @brief Wireless receiver bring-up entry.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "dac8562.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "sle_server.h"
#include "soc_osal.h"
#include "uart_handler.h"

static int sle_init_task(void *arg);

static void laser_wireless_receiver_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Wireless Laser Receiver Bring-up\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wireless rx] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wireless rx] laser init failed: 0x%x\r\n", ret);
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
        osal_printk("[laser wireless rx] create uart task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_UART) != OSAL_SUCCESS) {
        osal_printk("[laser wireless rx] set uart priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wireless rx] create motion task failed: 0x%x\r\n", ret);
    }

    osal_kthread_lock();
    task = osal_kthread_create(sle_init_task, NULL, "laser_sle", TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[laser wireless rx] create sle task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_SLE) != OSAL_SUCCESS) {
        osal_printk("[laser wireless rx] set sle priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[laser wireless rx] ready\r\n");
}

static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    errcode_t ret = sle_laser_server_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[laser wireless rx] SLE init failed: 0x%x\r\n", ret);
        return -1;
    }
    return OSAL_SUCCESS;
}

app_run(laser_wireless_receiver_entry);
