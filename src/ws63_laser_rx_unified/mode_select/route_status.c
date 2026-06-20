/**
 * @file route_status.c
 * @brief Route status logging helpers.
 */
#include "route_status.h"
#include "laser_ctrl.h"
#include "route_manager.h"
#include "soc_osal.h"

void route_status_print_boot_summary(void)
{
    osal_printk("[RX_INTEGRATED] active_route=%s\r\n",
                rx_route_name(route_manager_get_active()));
    osal_printk("[RX_INTEGRATED] recommended_route=%s\r\n",
                rx_route_name(route_manager_get_recommended()));
    osal_printk("[RX_INTEGRATED] compiled_routes=LEGACY_UART,LEGACY_WIFI,SLE_JOB\r\n");
    osal_printk("[RX_INTEGRATED] laser=%s\r\n", laser_is_enabled() ? "ON" : "OFF");
}
