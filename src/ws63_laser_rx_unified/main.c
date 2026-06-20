/**
 * @file main.c
 * @brief Unified RX Phase 2A entry: USART Direct stream.
 */
#include "app_init.h"
#include "common_def.h"
#include "dac8562.h"
#include "diagnostics/diag_log.h"
#include "gcode_processor.h"
#include "laser_ctrl.h"
#include "motion_executor.h"
#include "rx_core.h"
#include "rx_mode.h"
#include "rx_status.h"
#include "safety.h"
#include "soc_osal.h"
#include "uart_transport.h"

static void laser_rx_unified_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser RX Unified\r\n");
    osal_printk("  Phase 2A: USART Direct stream\r\n");
    osal_printk("========================================\r\n");
    osal_printk("[RX_UNIFIED] phase=2A uart direct\r\n");

    diag_log_info("boot begin");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_UNIFIED] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_UNIFIED] laser init failed: 0x%x\r\n", ret);
        return;
    }

    gcode_processor_init();
    motion_executor_init();
    safety_init();
    rx_core_init();
    rx_mode_set(RX_MODE_UART_DIRECT);
    safety_force_laser_off();

    ret = motion_executor_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_UNIFIED] motion task failed: 0x%x\r\n", ret);
        safety_abort_all();
        return;
    }

    ret = uart_transport_init();
    if (ret != ERRCODE_SUCC) {
        safety_abort_all();
        return;
    }

    ret = uart_transport_start_task();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_UNIFIED] uart task failed: 0x%x\r\n", ret);
        safety_abort_all();
        return;
    }

    osal_printk("[RX_UNIFIED] uart transport enabled\r\n");
    rx_status_print();
}

app_run(laser_rx_unified_entry);
