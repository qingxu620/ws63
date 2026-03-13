/**
 * @file main.c
 * @brief 接收板主入口 — 创建所有 RTOS 任务
 */
#include "app_init.h"
#include "soc_osal.h"
#include "common_def.h"
#include "config.h"

#include "dac8562.h"
#include "cmd_queue.h"
#include "interpolator.h"
#include "laser_ctrl.h"
#include "safety_monitor.h"
#include "sle_server.h"

/* 前向声明 */
static int sle_init_task(void *arg);

static void receiver_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser Marker - Receiver Board\r\n");
    osal_printk("========================================\r\n");

    /* 1. 硬件初始化 */
    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[receiver] dac8562 init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[receiver] laser ctrl init failed: 0x%x\r\n", ret);
        return;
    }

    interpolator_init();
    safety_monitor_init();
    if (cmd_queue_init() != 0) {
        osal_printk("[receiver] cmd queue init failed\r\n");
        return;
    }

    osal_printk("[receiver] hardware init OK\r\n");

    osal_task *task = NULL;

    /* 2. 创建插补引擎任务 (最高优先级) */
    osal_kthread_lock();
    task = osal_kthread_create(task_interpolator_entry, NULL, "interp", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[receiver] create interp task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_INTERPOLATOR) != OSAL_SUCCESS) {
        osal_printk("[receiver] set interp priority failed\r\n");
    }
    osal_kfree(task);

    /* 3. 创建安全监控任务 */
    task = osal_kthread_create(task_safety_entry, NULL, "safety", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[receiver] create safety task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_SAFETY) != OSAL_SUCCESS) {
        osal_printk("[receiver] set safety priority failed\r\n");
    }
    osal_kfree(task);

    /* 4. 创建 SLE 初始化任务 */
    task = osal_kthread_create(sle_init_task, NULL, "sle_init", TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[receiver] create sle_init task failed\r\n");
        return;
    }
    if (osal_kthread_set_priority(task, TASK_PRIO_SLE) != OSAL_SUCCESS) {
        osal_printk("[receiver] set sle priority failed\r\n");
    }
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[receiver] all tasks created\r\n");
}

static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500); /* 等待系统稳定 */
    errcode_t ret = sle_laser_server_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[receiver] SLE init failed: 0x%x\r\n", ret);
        return -1;
    }
    return OSAL_SUCCESS;
}

/* 系统启动入口 */
app_run(receiver_entry);
