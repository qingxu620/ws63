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
#include <stdio.h>
#include <string.h>

static volatile rx_route_t g_active_route = RX_ROUTE_NONE;
static volatile rx_route_t g_recommended_route = RX_ROUTE_SLE_JOB;
static volatile uint32_t g_switch_count = 0;

static bool route_manager_active_route_busy_known(bool *known)
{
    *known = true;
    switch (g_active_route) {
        case RX_ROUTE_LEGACY_UART:
#if defined(CONFIG_LASER_RX_TRANSPORT_UART)
            return !legacy_uart_route_is_idle();
#else
            *known = false;
            return true;
#endif
        case RX_ROUTE_LEGACY_WIFI:
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
            return !legacy_wifi_route_is_idle();
#else
            *known = false;
            return true;
#endif
        case RX_ROUTE_SLE_JOB:
#if defined(CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
            return !sle_job_route_is_idle();
#else
            *known = false;
            return true;
#endif
        case RX_ROUTE_NONE:
        case RX_ROUTE_SAFE:
            return false;
        default:
            *known = false;
            return true;
    }
}

static bool route_manager_active_route_busy(void)
{
    bool known;
    return route_manager_active_route_busy_known(&known);
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

rx_mode_t route_manager_get_mode(void)
{
    switch (g_active_route) {
        case RX_ROUTE_LEGACY_UART:
        case RX_ROUTE_LEGACY_WIFI:
            return RX_MODE_GRBL_STREAM;
        case RX_ROUTE_SLE_JOB:
            return RX_MODE_SLE_JOB;
        case RX_ROUTE_NONE:
        case RX_ROUTE_SAFE:
        default:
            return RX_MODE_NONE;
    }
}

const char *route_manager_mode_name(rx_mode_t mode)
{
    switch (mode) {
        case RX_MODE_NONE:
            return "NONE";
        case RX_MODE_GRBL_STREAM:
            return "GRBL_STREAM";
        case RX_MODE_SLE_JOB:
            return "SLE_JOB";
        default:
            return "UNKNOWN";
    }
}

const char *route_manager_switch_block_reason_name(rx_switch_block_reason_t reason)
{
    switch (reason) {
        case RX_SWITCH_BLOCK_NONE:
            return "NONE";
        case RX_SWITCH_BLOCK_LASER_ON:
            return "LASER_ON";
        case RX_SWITCH_BLOCK_ROUTE_BUSY:
            return "ROUTE_BUSY";
        case RX_SWITCH_BLOCK_UNKNOWN_BUSY:
            return "UNKNOWN_BUSY";
        default:
            return "UNKNOWN";
    }
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

void route_manager_get_status_snapshot(rx_mode_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->mode = route_manager_get_mode();
    out_status->active_route = g_active_route;
    out_status->recommended_route = g_recommended_route;
#if defined(CONFIG_LASER_RX_TRANSPORT_UART)
    out_status->compiled_uart = true;
#endif
#if defined(CONFIG_LASER_RX_TRANSPORT_WIFI)
    out_status->compiled_wifi = true;
#endif
#if defined(CONFIG_LASER_RX_TRANSPORT_SLE_JOB)
    out_status->compiled_sle_job = true;
#endif
    out_status->laser_on = laser_is_enabled();
    out_status->laser_off = !out_status->laser_on;

    bool busy_known = false;
    out_status->busy = route_manager_active_route_busy_known(&busy_known);
    if (out_status->laser_on) {
        out_status->switch_block_reason = RX_SWITCH_BLOCK_LASER_ON;
    } else if (!busy_known) {
        out_status->busy = true;
        out_status->switch_block_reason = RX_SWITCH_BLOCK_UNKNOWN_BUSY;
    } else if (out_status->busy) {
        out_status->switch_block_reason = RX_SWITCH_BLOCK_ROUTE_BUSY;
    } else {
        out_status->switchable = true;
        out_status->switch_block_reason = RX_SWITCH_BLOCK_NONE;
    }
    out_status->switch_count = g_switch_count;
}

void route_manager_dump_status(void)
{
    rx_mode_status_t status;
    route_manager_get_status_snapshot(&status);

    char compiled[32];
    const char *uart = status.compiled_uart ? "UART" : "";
    const char *wifi_sep = status.compiled_wifi && status.compiled_uart ? "," : "";
    const char *wifi = status.compiled_wifi ? "WIFI" : "";
    const char *sle_sep = status.compiled_sle_job &&
                          (status.compiled_uart || status.compiled_wifi) ? "," : "";
    const char *sle = status.compiled_sle_job ? "SLE_JOB" : "";
    int ret = snprintf(compiled, sizeof(compiled), "%s%s%s%s%s",
                       uart, wifi_sep, wifi, sle_sep, sle);
    if (ret <= 0 || compiled[0] == '\0') {
        (void)snprintf(compiled, sizeof(compiled), "NONE");
    }

    osal_printk("[RX_MODE] mode=%s route=%s recommended=%s laser=%s busy=%d switchable=%d reason=%s switches=%u\r\n",
                route_manager_mode_name(status.mode), rx_route_name(status.active_route),
                rx_route_name(status.recommended_route), status.laser_on ? "ON" : "OFF",
                status.busy ? 1 : 0, status.switchable ? 1 : 0,
                route_manager_switch_block_reason_name(status.switch_block_reason),
                (unsigned int)status.switch_count);
    osal_printk("[RX_MODE] compiled=%s\r\n", compiled);
    osal_printk("[RX_MODE] grbl_stream=planned uart_wifi_dual_frontend owner=disabled\r\n");
}
