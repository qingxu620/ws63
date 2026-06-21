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
rx_mode_t route_manager_get_mode(void);
const char *route_manager_mode_name(rx_mode_t mode);
const char *route_manager_switch_block_reason_name(rx_switch_block_reason_t reason);
bool route_manager_can_switch(rx_route_t route);
bool route_manager_set_active(rx_route_t route);
bool route_manager_is_switching(void);
bool route_manager_can_request_switch(rx_route_t target);
bool route_manager_request_safe_switch(rx_route_t target);
void route_manager_get_status(rx_route_status_t *out_status);
void route_manager_print_status(void);
void route_manager_get_status_snapshot(rx_mode_status_t *out_status);
void route_manager_dump_status(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_ROUTE_MANAGER_H */
