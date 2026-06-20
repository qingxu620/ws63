/**
 * @file wifi_grbl_server.h
 * @brief WiFi TCP Grbl server for the WS63 laser marker sample.
 */
#ifndef LEGACY_WIFI_ROUTE_H
#define LEGACY_WIFI_ROUTE_H

#include "errcode.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t legacy_wifi_route_init(void);
errcode_t legacy_wifi_route_start(void);
bool legacy_wifi_route_is_idle(void);
void legacy_wifi_route_force_stop(void);
int legacy_wifi_route_task_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_WIFI_ROUTE_H */
