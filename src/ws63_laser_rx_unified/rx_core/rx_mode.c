/**
 * @file rx_mode.c
 * @brief Unified RX mode state.
 */
#include "rx_mode.h"

static volatile rx_mode_t g_mode = RX_MODE_IDLE;

void rx_mode_init(void)
{
    g_mode = RX_MODE_IDLE;
}

void rx_mode_set(rx_mode_t mode)
{
    g_mode = mode;
}

rx_mode_t rx_mode_get(void)
{
    return g_mode;
}

const char *rx_mode_name(rx_mode_t mode)
{
    switch (mode) {
        case RX_MODE_IDLE: return "IDLE";
        case RX_MODE_UART_DIRECT: return "UART";
        case RX_MODE_WIFI_TCP: return "WIFI_TCP";
        case RX_MODE_SLE_JOB: return "SLE_JOB";
        case RX_MODE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
