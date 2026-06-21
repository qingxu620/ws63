/**
 * @file route_types.h
 * @brief Route identifiers for the integrated RX firmware.
 */
#ifndef WS63_LASER_RX_UNIFIED_ROUTE_TYPES_H
#define WS63_LASER_RX_UNIFIED_ROUTE_TYPES_H

#include "rx_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RX_ROUTE_NONE = 0,
    RX_ROUTE_LEGACY_UART,
    RX_ROUTE_LEGACY_WIFI,
    RX_ROUTE_SLE_JOB,
    RX_ROUTE_SAFE,
} rx_route_t;

typedef struct {
    rx_route_t active;
    rx_route_t recommended;
    bool laser_off;
    bool route_busy;
    uint32_t switch_count;
} rx_route_status_t;

typedef enum {
    RX_SWITCH_BLOCK_NONE = 0,
    RX_SWITCH_BLOCK_LASER_ON,
    RX_SWITCH_BLOCK_ROUTE_BUSY,
    RX_SWITCH_BLOCK_UNKNOWN_BUSY,
} rx_switch_block_reason_t;

typedef struct {
    rx_mode_t mode;
    rx_route_t active_route;
    rx_route_t recommended_route;
    bool compiled_uart;
    bool compiled_wifi;
    bool compiled_sle_job;
    bool laser_on;
    bool laser_off;
    bool busy;
    bool switchable;
    rx_switch_block_reason_t switch_block_reason;
    uint32_t switch_count;
} rx_mode_status_t;

const char *rx_route_name(rx_route_t route);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_ROUTE_TYPES_H */
