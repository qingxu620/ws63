/**
 * @file route_manager.h
 * @brief Stub route manager for route-based RX integration.
 */
#ifndef WS63_LASER_RX_UNIFIED_ROUTE_MANAGER_H
#define WS63_LASER_RX_UNIFIED_ROUTE_MANAGER_H

#include "route_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void route_manager_init(void);
rx_route_t route_manager_get_active(void);
rx_route_t route_manager_get_recommended(void);
bool route_manager_can_switch(rx_route_t route);
bool route_manager_set_active(rx_route_t route);
void route_manager_get_status(rx_route_status_t *out_status);
void route_manager_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_ROUTE_MANAGER_H */
