/**
 * @file main.c
 * @brief Wireless transmitter bring-up entry.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "gcode_processor.h"
#include "protocol.h"
#include "sle_errcode.h"
#include "sle_client.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart_handler.h"
#include "wireless_crc16.h"
#include <stdbool.h>
#include <string.h>

static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    errcode_t ret = sle_laser_client_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[wireless tx] SLE init failed: 0x%x\r\n", ret);
        return -1;
    }
    return OSAL_SUCCESS;
}

static int heartbeat_task(void *arg)
{
    unused(arg);
    motion_cmd_t hb = {0};
    while (1) {
        uint32_t sleep_ms = HEARTBEAT_INTERVAL_MS;
        uint32_t now = (uint32_t)uapi_systick_get_ms();
        uint32_t last_business = sle_laser_client_get_last_business_write_ms();
        bool suppress = (last_business != 0U) &&
                        ((uint32_t)(now - last_business) < SLE_TX_HEARTBEAT_SUPPRESS_AFTER_BUSINESS_MS);

        if (sle_laser_client_can_send_heartbeat() && !suppress) {
            memset(&hb, 0, sizeof(hb));
            hb.cmd = CMD_HEARTBEAT;
            motion_cmd_set_crc(&hb);
            errcode_t ret = sle_laser_client_send_cmd(&hb);
            if (ret == ERRCODE_SLE_BUSY) {
                sleep_ms = SLE_TX_HEARTBEAT_BUSY_RETRY_INTERVAL_MS;
            }
        }
        osal_msleep(sleep_ms);
    }
    return 0;
}

static void laser_wireless_transmitter_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Wireless Laser Transmitter Bring-up\r\n");
    osal_printk("========================================\r\n");

    gcode_processor_init();
    if (uart_handler_init() != ERRCODE_SUCC) {
        return;
    }

    osal_kthread_lock();
    osal_task *task = osal_kthread_create(task_uart_rx_entry, NULL, "tx_uart", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_UART);
        osal_kfree(task);
    }
    task = osal_kthread_create(task_tx_sender_entry, NULL, "tx_sender", TASK_STACK_SIZE_DEFAULT);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }
    task = osal_kthread_create(sle_init_task, NULL, "tx_sle", TASK_STACK_SIZE_SLE);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }
    task = osal_kthread_create(heartbeat_task, NULL, "tx_hb", TASK_STACK_SIZE_SLE);
    if (task != NULL) {
        osal_kthread_set_priority(task, TASK_PRIO_SLE);
        osal_kfree(task);
    }
    osal_kthread_unlock();

    osal_printk("[wireless tx] ready\r\n");
}

app_run(laser_wireless_transmitter_entry);
