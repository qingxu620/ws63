/**
 * @file route_types.h
 * @brief Route identifiers for the integrated RX firmware.
 */
#ifndef WS63_LASER_RX_UNIFIED_ROUTE_TYPES_H
#define WS63_LASER_RX_UNIFIED_ROUTE_TYPES_H

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

const char *rx_route_name(rx_route_t route);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_ROUTE_TYPES_H */
