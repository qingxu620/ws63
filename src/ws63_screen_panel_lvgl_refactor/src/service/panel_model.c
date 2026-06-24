/**
 * @file panel_model.c
 * @brief Local state model with demo state cycling.
 */
#include "panel_model.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

panel_model_t g_model;

void panel_model_init(void)
{
    memset(&g_model, 0, sizeof(g_model));
    g_model.state = SYS_STATE_NO_JOB;
    g_model.rx_connected = true;
    g_model.sle_connected = true;
    g_model.sd_mounted = false;
    snprintf(g_model.job_name, sizeof(g_model.job_name), "暂无任务");
    osal_printk("[MODEL] init state=NO_JOB\r\n");
}

void panel_model_set_state(system_state_t state)
{
    if (state >= SYS_STATE_COUNT) return;
    g_model.state = state;
    g_model.job_seconds = 0;

    switch (state) {
    case SYS_STATE_NO_JOB:
        g_model.progress = 0;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "暂无任务");
        break;
    case SYS_STATE_RECEIVING:
        g_model.progress = 0;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "TASK_001");
        break;
    case SYS_STATE_READY:
        g_model.progress = 0;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "TASK_001");
        break;
    case SYS_STATE_RUNNING:
        snprintf(g_model.job_name, sizeof(g_model.job_name), "TASK_001");
        break;
    case SYS_STATE_DONE:
        g_model.progress = 100;
        break;
    case SYS_STATE_ERROR:
        g_model.progress = 0;
        break;
    case SYS_STATE_LINK_LOST:
        g_model.rx_connected = false;
        g_model.progress = 0;
        break;
    default:
        break;
    }
    osal_printk("[MODEL] state=%d progress=%d\r\n", g_model.state, g_model.progress);
}

void panel_model_set_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_model.progress = pct;
}

void panel_model_tick(void)
{
    if (g_model.state == SYS_STATE_RUNNING) {
        g_model.job_seconds++;
    }
}
