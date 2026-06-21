/**
 * @file route_manager.c
 * @brief Stub route manager for route-based RX integration.
 */
#include "route_manager.h"
#include "laser_ctrl.h"
#include "soc_osal.h"
#if defined(CONFIG_LASER_RX_TRANSPORT_UART)
#include "legacy_uart_route.h"
#endif
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
#include "legacy_wifi_route.h"
#endif
#if defined(CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
#include "sle_job_route.h"
#endif
#include <string.h>

static volatile rx_route_t g_active_route = RX_ROUTE_NONE;
static volatile rx_route_t g_recommended_route = RX_ROUTE_SLE_JOB;
static volatile uint32_t g_switch_count = 0;

static bool route_manager_active_route_busy(void)
{
    switch (g_active_route) {
        case RX_ROUTE_LEGACY_UART:
#if defined(CONFIG_LASER_RX_TRANSPORT_UART)
            return !legacy_uart_route_is_idle();
#else
            return false;
#endif
        case RX_ROUTE_LEGACY_WIFI:
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
            return !legacy_wifi_route_is_idle();
#else
            return false;
#endif
        case RX_ROUTE_SLE_JOB:
#if defined(CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
            return !sle_job_route_is_idle();
#else
            return false;
#endif
        default:
            return false;
    }
}

const char *rx_route_name(rx_route_t route)
{
    switch (route) {
        case RX_ROUTE_NONE:
            return "NONE";
        case RX_ROUTE_LEGACY_UART:
            return "LEGACY_UART";
        case RX_ROUTE_LEGACY_WIFI:
            return "LEGACY_WIFI";
        case RX_ROUTE_SLE_JOB:
            return "SLE_JOB";
        case RX_ROUTE_SAFE:
            return "SAFE";
        default:
            return "UNKNOWN";
    }
}

void route_manager_init(void)
{
    g_active_route = RX_ROUTE_NONE;
    g_recommended_route = RX_ROUTE_SLE_JOB;
    g_switch_count = 0;
    osal_printk("[ROUTE] manager init active=%s recommended=%s\r\n",
                rx_route_name(g_active_route), rx_route_name(g_recommended_route));
}

rx_route_t route_manager_get_active(void)
{
    return g_active_route;
}

rx_route_t route_manager_get_recommended(void)
{
    return g_recommended_route;
}

bool route_manager_can_switch(rx_route_t route)
{
    if (route != RX_ROUTE_NONE && route != RX_ROUTE_LEGACY_UART &&
        route != RX_ROUTE_LEGACY_WIFI && route != RX_ROUTE_SLE_JOB &&
        route != RX_ROUTE_SAFE) {
        return false;
    }

    if (laser_is_enabled()) {
        return false;
    }

    if (g_active_route != RX_ROUTE_NONE && g_active_route != RX_ROUTE_SAFE) {
        return false;
    }

    if (route_manager_active_route_busy()) {
        return false;
    }

    return true;
}

bool route_manager_set_active(rx_route_t route)
{
    if (!route_manager_can_switch(route)) {
        osal_printk("[ROUTE] switch reject target=%s active=%s\r\n",
                    rx_route_name(route), rx_route_name(g_active_route));
        return false;
    }

    if (route == RX_ROUTE_LEGACY_UART) {
#if defined(CONFIG_LASER_RX_TRANSPORT_UART)
        osal_printk("[ROUTE] start LEGACY_UART\r\n");
        errcode_t ret = legacy_uart_route_start();
        if (ret != ERRCODE_SUCC) {
            osal_printk("[ROUTE] start LEGACY_UART failed: 0x%x\r\n", ret);
            return false;
        }
#else
        osal_printk("[ROUTE] start LEGACY_UART failed: transport disabled\r\n");
        return false;
#endif
    }

    if (route == RX_ROUTE_LEGACY_WIFI) {
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
        osal_printk("[ROUTE] start LEGACY_WIFI\r\n");
        errcode_t ret = legacy_wifi_route_start();
        if (ret != ERRCODE_SUCC) {
            osal_printk("[ROUTE] start LEGACY_WIFI failed: 0x%x\r\n", ret);
            laser_force_off();
            return false;
        }
#else
        osal_printk("[ROUTE] start LEGACY_WIFI failed: transport disabled\r\n");
        return false;
#endif
    }

    if (route == RX_ROUTE_SLE_JOB) {
#if defined(CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
        osal_printk("[ROUTE] start SLE_JOB\r\n");
        errcode_t ret = sle_job_route_start();
        if (ret != ERRCODE_SUCC) {
            osal_printk("[ROUTE] start SLE_JOB failed: 0x%x\r\n", ret);
            laser_force_off();
            return false;
        }
#else
        osal_printk("[ROUTE] start SLE_JOB failed: transport disabled\r\n");
        return false;
#endif
    }

    g_active_route = route;
    g_switch_count++;
    osal_printk("[ROUTE] active=%s switch_count=%u\r\n",
                rx_route_name(g_active_route), (unsigned int)g_switch_count);
    route_manager_print_status();
    return true;
}

void route_manager_get_status(rx_route_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->active = g_active_route;
    out_status->recommended = g_recommended_route;
    out_status->laser_off = !laser_is_enabled();
    out_status->route_busy = route_manager_active_route_busy();
    out_status->switch_count = g_switch_count;
}

void route_manager_print_status(void)
{
    rx_route_status_t status;
    route_manager_get_status(&status);
    osal_printk("[RX_ROUTE] active=%s recommended=%s laser=%s busy=%d switches=%u\r\n",
                rx_route_name(status.active), rx_route_name(status.recommended),
                status.laser_off ? "OFF" : "ON", status.route_busy ? 1 : 0,
                (unsigned int)status.switch_count);
}
