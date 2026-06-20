/**
 * @file main.c
 * @brief Integrated RX route-based R3A entry.
 */
#include "app_init.h"
#include "dac8562.h"
#include "laser_ctrl.h"
#include "route_manager.h"
#include "route_status.h"
#include "soc_osal.h"

static void laser_rx_unified_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser RX Integrated\r\n");
    osal_printk("  Route-based integration R3A\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = dac8562_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_INTEGRATED] dac init failed: 0x%x\r\n", ret);
        return;
    }

    ret = laser_ctrl_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[RX_INTEGRATED] laser init failed: 0x%x\r\n", ret);
        return;
    }

    laser_force_off();
    route_manager_init();

    route_status_print_boot_summary();
    route_manager_print_status();
#if defined(CONFIG_LASER_RX_TRANSPORT_UART)
    osal_printk("[RX_INTEGRATED] R3A start legacy_uart route\r\n");
    if (!route_manager_set_active(RX_ROUTE_LEGACY_UART)) {
        osal_printk("[RX_INTEGRATED] start legacy_uart failed\r\n");
        laser_force_off();
        return;
    }
    osal_printk("[RX_INTEGRATED] active_route=%s\r\n", rx_route_name(route_manager_get_active()));
    osal_printk("[RX_INTEGRATED] laser=%s\r\n", laser_is_enabled() ? "ON" : "OFF");
#endif
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
    osal_printk("[RX_INTEGRATED] R3A legacy_wifi compiled but not started\r\n");
#endif
    osal_printk("[RX_INTEGRATED] WiFi/SLE route not started\r\n");
}

app_run(laser_rx_unified_entry);
