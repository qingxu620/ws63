/**
 * @file legacy_uart_route.h
 * @brief LaserGRBL UART entry for the integrated RX legacy UART route.
 */
#ifndef LEGACY_UART_ROUTE_H
#define LEGACY_UART_ROUTE_H

#include "errcode.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t legacy_uart_route_init(void);
errcode_t legacy_uart_route_start(void);
bool legacy_uart_route_is_idle(void);
void legacy_uart_route_force_stop(void);
int legacy_uart_route_task_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_UART_ROUTE_H */
