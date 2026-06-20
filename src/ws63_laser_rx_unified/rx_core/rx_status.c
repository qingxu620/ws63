/**
 * @file rx_status.c
 * @brief Unified RX status snapshot.
 */
#include "rx_status.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "rx_mode.h"
#include "soc_osal.h"
#include <string.h>

void rx_status_init(void)
{
}

void rx_status_get(rx_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->mode = rx_mode_get();
    out_status->laser_on = laser_is_enabled();
    out_status->motion_busy = motion_executor_is_busy();
    out_status->motion_queue_depth = motion_executor_queue_depth();
    out_status->executed_count = (uint32_t)motion_executor_executed_count();
}

void rx_status_print(void)
{
    rx_status_t status;
    rx_status_get(&status);
    osal_printk("[RX_STATUS] mode=%s laser=%s motion_busy=%d queue=%u executed=%u\r\n",
                rx_mode_name(status.mode),
                status.laser_on ? "ON" : "OFF",
                status.motion_busy ? 1 : 0,
                (unsigned int)status.motion_queue_depth,
                (unsigned int)status.executed_count);
}
