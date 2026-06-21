/**
 * @file sle_job_route.c
 * @brief Integrated SLE job route lifecycle.
 */
#include "sle_job_route.h"
#include "common_def.h"
#include "laser_ctrl.h"
#include "sle_job_config.h"
#include "sle_job_gcode_processor.h"
#include "sle_job_manager.h"
#include "sle_job_motion_executor.h"
#include "sle_job_route_server.h"
#include "soc_osal.h"

static volatile bool g_route_started = false;
static volatile bool g_server_ready = false;
static volatile bool g_server_failed = false;

static int sle_job_route_init_task(void *arg)
{
    unused(arg);

    osal_printk("[SLE_JOB_ROUTE] init task start, delay for stack ready\r\n");
    osal_msleep(500);

    osal_printk("[SLE_JOB_ROUTE] server init begin\r\n");
    errcode_t ret = sle_job_route_server_init();
    osal_printk("[SLE_JOB_ROUTE] server init end ret=0x%x\r\n", ret);
    if (ret != ERRCODE_SUCC) {
        g_server_failed = true;
        g_server_ready = false;
        sle_job_manager_safe_stop("sle-init-fail");
        return -1;
    }

    g_server_ready = true;
    g_server_failed = false;
    osal_printk("[SLE_JOB_ROUTE] server ready\r\n");
    return 0;
}

errcode_t sle_job_route_start(void)
{
    if (g_route_started) {
        return ERRCODE_SUCC;
    }

    laser_force_off();
    g_server_ready = false;
    g_server_failed = false;

    osal_printk("[SLE_JOB_ROUTE] gcode/motion/job init begin\r\n");
    sle_job_gcode_processor_init();
    sle_job_motion_executor_init();
    sle_job_manager_init();

    osal_printk("[SLE_JOB_ROUTE] callbacks set\r\n");
    sle_job_route_server_set_packet_cb(sle_job_manager_on_packet);
    sle_job_route_server_set_disconnect_cb(sle_job_manager_on_disconnect);

    osal_printk("[SLE_JOB_ROUTE] motion task start\r\n");
    errcode_t ret = sle_job_motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE_JOB_ROUTE] motion task failed: 0x%x\r\n", ret);
        sle_job_manager_safe_stop("motion-task-fail");
        return ret;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(sle_job_route_init_task, NULL, "sle_job_init",
                                          SLE_JOB_TASK_STACK_SIZE_SLE);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[SLE_JOB_ROUTE] create init task failed\r\n");
        sle_job_manager_safe_stop("sle-task-fail");
        return ERRCODE_FAIL;
    }
    if (osal_kthread_set_priority(task, SLE_JOB_TASK_PRIO_SLE) != OSAL_SUCCESS) {
        osal_printk("[SLE_JOB_ROUTE] set init priority failed\r\n");
    }
    g_route_started = true;
    osal_kfree(task);
    osal_kthread_unlock();

    osal_printk("[SLE_JOB_ROUTE] route started, server init pending\r\n");
    return ERRCODE_SUCC;
}

bool sle_job_route_is_idle(void)
{
    if (!g_route_started || !g_server_ready || g_server_failed) {
        return false;
    }
    return sle_job_manager_is_idle() && !sle_job_motion_executor_is_busy() &&
           sle_job_motion_executor_queue_depth() == 0 && !laser_is_enabled();
}

void sle_job_route_force_stop(void)
{
    if (g_route_started) {
        sle_job_manager_safe_stop("route-force-stop");
    }
    laser_force_off();
}
