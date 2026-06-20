/**
 * @file safety.c
 * @brief Unified RX safety helpers.
 */
#include "safety.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "rx_mode.h"
#include "soc_osal.h"

void safety_init(void)
{
    safety_force_laser_off();
    osal_printk("[SAFETY] init laser=OFF\r\n");
}

void safety_force_laser_off(void)
{
    laser_force_off();
}

void safety_abort_all(void)
{
    motion_cmd_t cmd;

    osal_printk("[SAFETY_ABORT_ALL] mode=%s\r\n", rx_mode_name(rx_mode_get()));
    laser_force_off();
    motion_executor_request_abort();
    motion_executor_flush();
    gcode_processor_build_emergency_stop(&cmd);
    motion_executor_execute(&cmd);
    laser_force_off();
    rx_mode_set(RX_MODE_ERROR);
}

void safety_on_disconnect(void)
{
    osal_printk("[SAFETY_DISCONNECT] mode=%s\r\n", rx_mode_name(rx_mode_get()));
    safety_abort_all();
    rx_mode_set(RX_MODE_IDLE);
}
