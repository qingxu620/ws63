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
static volatile bool g_server_initialized_once = false;

static errcode_t sle_job_route_activate_server(const char *reason)
{
    bool reused = g_server_initialized_once;
    errcode_t ret = sle_job_route_server_init();
    if (ret != ERRCODE_SUCC) {
        g_server_failed = true;
        g_server_ready = false;
        osal_printk("[SLE_JOB_ROUTE] server activate failed reason=%s ret=0x%x\r\n",
                    (reason != NULL) ? reason : "unspecified", ret);
        sle_job_manager_safe_stop("sle-init-fail");
        return ret;
    }

    g_server_initialized_once = true;
    g_server_ready = true;
    g_server_failed = false;
    osal_printk("[SLE_JOB_ROUTE] server ready reason=%s reused=%u\r\n",
                (reason != NULL) ? reason : "unspecified",
                reused ? 1U : 0U);
    return ERRCODE_SUCC;
}

static int sle_job_route_init_task(void *arg)
{
    unused(arg);

    osal_msleep(500);
    return (sle_job_route_activate_server("initial") == ERRCODE_SUCC) ? 0 : -1;
}

errcode_t sle_job_route_start(void)
{
    if (g_route_started) {
        return ERRCODE_SUCC;
    }

    laser_force_off();
    g_server_ready = false;
    g_server_failed = false;

    sle_job_gcode_processor_init();
    sle_job_motion_executor_init();
    sle_job_manager_init();

    sle_job_route_server_set_packet_cb(sle_job_manager_on_packet);
    sle_job_route_server_set_disconnect_cb(sle_job_manager_on_disconnect);

    errcode_t ret = sle_job_motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE_JOB_ROUTE] motion task failed: 0x%x\r\n", ret);
        sle_job_manager_safe_stop("motion-task-fail");
        return ret;
    }

    /*
     * The SLE server/service is intentionally configured only once.  After a
     * WiFi -> SLE route switch, server_init() only re-enables the persistent
     * RX work queue and resumes advertising, so do that synchronously instead
     * of allocating another one-shot 16 KB init task while WiFi tasks are
     * still unwinding.
     */
    if (g_server_initialized_once) {
        ret = sle_job_route_activate_server("resume");
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        g_route_started = true;
        return ERRCODE_SUCC;
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

bool sle_job_route_is_server_ready(void)
{
    return g_route_started && g_server_ready && !g_server_failed;
}

bool sle_job_route_server_failed(void)
{
    return g_server_failed;
}

bool sle_job_route_is_connected(void)
{
    return g_route_started && sle_job_route_server_is_connected();
}

void sle_job_route_force_stop(void)
{
    if (g_route_started) {
        sle_job_manager_safe_stop("route-force-stop");
        (void)sle_job_route_server_stop();
    }
    laser_force_off();
    g_route_started = false;
    g_server_ready = false;
    g_server_failed = false;
}
